namespace Omacut.Core;

/// <summary>
/// The clips being edited, with undo. A gesture (a drag) is one undo step.
/// Times called t are seconds along the edited sequence. Mirrors omacut's Timeline.
/// </summary>
public sealed class Timeline
{
    private readonly List<double> _sourceDurations = [];
    private List<Clip> _clips = [];
    private List<Clip> _exported = [];
    private readonly List<List<Clip>> _undo = [];
    private readonly List<List<Clip>> _redo = [];
    private bool _gesture;
    private bool _gestureRecorded;

    public event EventHandler? Changed;

    public IReadOnlyList<Clip> Clips => _clips;
    public double Duration => Edit.Duration(_clips);
    public bool CanUndo => _undo.Count > 0;
    public bool CanRedo => _redo.Count > 0;

    public bool Unexported =>
        _clips.Count > 0 &&
        !Edit.Untouched(_clips, _sourceDurations.Count > 0 ? _sourceDurations[0] : 0) &&
        !Same(_clips, _exported);

    /// <summary>A freshly loaded video: one clip spanning all of it, and no history.</summary>
    public void Reset(double duration)
    {
        duration = Math.Max(0, duration);
        _sourceDurations.Clear();
        _sourceDurations.Add(duration);
        _clips = duration > 0 ? [new Clip(0, 0, duration)] : [];
        _exported = [];
        _undo.Clear();
        _redo.Clear();
        _gesture = false;
        OnChanged();
    }

    /// <summary>
    /// Adds a video of the given length as a clip after the one under t
    /// (at the end, past the last). Returns its source index.
    /// </summary>
    public int AddSource(double duration, double t)
    {
        _sourceDurations.Add(Math.Max(0, duration));
        int source = _sourceDurations.Count - 1;
        var next = new List<Clip>(_clips);
        int at = _clips.Count == 0 ? 0 : ClipAt(t) + 1;
        next.Insert(at, new Clip(source, 0, _sourceDurations[^1]));
        Apply(next);
        return source;
    }

    public void MarkExported(IReadOnlyList<Clip> clips)
    {
        _exported = [.. clips];
        OnChanged();
    }

    /// <summary>The clip under t. The end of the sequence belongs to the last clip.</summary>
    public int ClipAt(double t)
    {
        double end = 0;
        for (int i = 0; i < _clips.Count; i++)
        {
            end += _clips[i].Length;
            if (t < end)
            {
                return i;
            }
        }

        return _clips.Count - 1;
    }

    public double ClipStart(int index)
    {
        double start = 0;
        for (int i = 0; i < index && i < _clips.Count; i++)
        {
            start += _clips[i].Length;
        }

        return start;
    }

    /// <summary>The nearest clip edge before (direction &lt; 0) or after t.</summary>
    public double EdgeFrom(double t, int direction)
    {
        double target = direction > 0 ? Duration : 0;
        double edge = 0;
        for (int i = 0; i <= _clips.Count; i++)
        {
            if (direction > 0 && edge > t + 0.01)
            {
                return edge;
            }

            if (direction < 0 && edge < t - 0.01)
            {
                target = edge;
            }

            if (i < _clips.Count)
            {
                edge += _clips[i].Length;
            }
        }

        return target;
    }

    public bool CanJoin(int index) =>
        index >= 0 && index + 1 < _clips.Count &&
        _clips[index].Source == _clips[index + 1].Source &&
        _clips[index].Out == _clips[index + 1].In;

    /// <summary>
    /// The next clip carries on in the same source, including a split's float noise.
    /// A real cut returns false and playback has to seek.
    /// </summary>
    public bool Continues(int index) =>
        index >= 0 && index + 1 < _clips.Count && Edit.Continues(_clips[index], _clips[index + 1]);

    public void Split(double t)
    {
        int i = ClipAt(t);
        if (i < 0)
        {
            return;
        }

        Clip clip = _clips[i];
        double at = clip.In + (t - ClipStart(i));
        if (at - clip.In < Edit.MinimumClip || clip.Out - at < Edit.MinimumClip)
        {
            return;
        }

        var next = new List<Clip>(_clips);
        next[i] = clip with { Out = at };
        next.Insert(i + 1, new Clip(clip.Source, at, clip.Out));
        Apply(next);
    }

    public void SetClip(int index, double start, double end)
    {
        if (index < 0 || index >= _clips.Count)
        {
            return;
        }

        double sourceEnd = SourceDuration(_clips[index].Source);
        start = Math.Clamp(start, 0, sourceEnd);
        end = Math.Clamp(end, 0, sourceEnd);
        if (end - start < Edit.MinimumClip)
        {
            return;
        }

        var next = new List<Clip>(_clips);
        next[index] = _clips[index] with { In = start, Out = end };
        Apply(next);
    }

    public void MoveEdge(int index, bool start, double sourceTime)
    {
        if (index < 0 || index >= _clips.Count)
        {
            return;
        }

        Clip clip = _clips[index];
        if (start)
        {
            SetClip(index, Math.Min(sourceTime, clip.Out - Edit.MinimumClip), clip.Out);
        }
        else
        {
            SetClip(index, clip.In, Math.Max(sourceTime, clip.In + Edit.MinimumClip));
        }
    }

    public void TrimTo(double t, bool start)
    {
        int i = ClipAt(t);
        if (i < 0)
        {
            return;
        }

        double at = _clips[i].In + (t - ClipStart(i));
        if (start)
        {
            SetClip(i, at, _clips[i].Out);
        }
        else
        {
            SetClip(i, _clips[i].In, at);
        }
    }

    public void RemoveClip(int index)
    {
        if (index < 0 || index >= _clips.Count || _clips.Count < 2)
        {
            return;
        }

        var next = new List<Clip>(_clips);
        next.RemoveAt(index);
        Apply(next);
    }

    public void RemoveAt(double t) => RemoveClip(ClipAt(t));

    public void MoveClip(int from, int to)
    {
        if (from < 0 || from >= _clips.Count || to < 0 || to >= _clips.Count || from == to)
        {
            return;
        }

        var next = new List<Clip>(_clips);
        Clip clip = next[from];
        next.RemoveAt(from);
        next.Insert(to, clip);
        Apply(next);
    }

    public void JoinClips(int index)
    {
        if (!CanJoin(index))
        {
            return;
        }

        var next = new List<Clip>(_clips);
        next[index] = next[index] with { Out = next[index + 1].Out };
        next.RemoveAt(index + 1);
        Apply(next);
    }

    public void BeginGesture()
    {
        if (_clips.Count == 0)
        {
            return;
        }

        _gesture = true;
        _gestureRecorded = false;
    }

    public void EndGesture() => _gesture = false;

    public void Undo()
    {
        if (_gesture || _undo.Count == 0)
        {
            return;
        }

        _redo.Add(_clips);
        _clips = _undo[^1];
        _undo.RemoveAt(_undo.Count - 1);
        OnChanged();
    }

    public void Redo()
    {
        if (_gesture || _redo.Count == 0)
        {
            return;
        }

        _undo.Add(_clips);
        _clips = _redo[^1];
        _redo.RemoveAt(_redo.Count - 1);
        OnChanged();
    }

    private double SourceDuration(int source) =>
        source >= 0 && source < _sourceDurations.Count ? _sourceDurations[source] : 0;

    private void Apply(List<Clip> next)
    {
        if (Same(next, _clips) || next.Count == 0)
        {
            return;
        }

        if (!_gesture || !_gestureRecorded)
        {
            _undo.Add(_clips);
            _redo.Clear();
            _gestureRecorded = _gesture;
        }

        _clips = next;
        OnChanged();
    }

    private static bool Same(List<Clip> left, List<Clip> right) => left.SequenceEqual(right);

    private void OnChanged() => Changed?.Invoke(this, EventArgs.Empty);
}
