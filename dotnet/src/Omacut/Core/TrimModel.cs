namespace Omacut.Core;

/// <summary>
/// The trim selection, playhead and zoom window, in seconds. Mirrors omacut's TrimBar/Main.qml
/// rules: the playhead lives inside the trim, edges keep a minimum gap, edges park the playhead
/// on themselves, and zoom frames the selection with slack on each side.
/// </summary>
public sealed class TrimModel
{
    public double Duration { get; private set; }
    public double Start { get; private set; }
    public double End { get; private set; }
    public double Playhead { get; private set; }
    public bool Zoomed { get; private set; }
    public double ViewStart { get; private set; }
    public double ViewEnd { get; private set; }

    /// <summary>The stretch of video the track currently shows.</summary>
    public double WindowStart => Zoomed ? ViewStart : 0;
    public double WindowEnd => Zoomed ? ViewEnd : Duration;
    public double WindowLength => Math.Max(WindowEnd - WindowStart, 0.001);
    public double Length => End - Start;
    public double MinGap => Math.Min(0.1, Duration);

    /// <summary>True when the selection is narrower than the whole video.</summary>
    public bool IsTrimmed => Duration > 0 && (Start > 0 || End < Duration);

    public event EventHandler? Changed;

    public void Reset(double duration)
    {
        Duration = Math.Max(duration, 0);
        Start = 0;
        End = Duration;
        Playhead = 0;
        Zoomed = false;
        ViewStart = 0;
        ViewEnd = Duration;
        OnChanged();
    }

    public void SetPlayhead(double seconds)
    {
        Playhead = Math.Clamp(seconds, Start, End);
        OnChanged();
    }

    /// <summary>Playhead within the trim, like scrubbing and preview.</summary>
    public double SeekBy(double delta)
    {
        SetPlayhead(Playhead + delta);
        return Playhead;
    }

    // While zoomed, the edges stop at the zoom window instead of the video bounds.
    public void MoveStartTo(double seconds, bool parkPlayhead = true)
    {
        if (Duration <= 0)
        {
            return;
        }

        Start = Math.Max(WindowStart, Math.Min(seconds, End - MinGap));
        if (parkPlayhead || Playhead < Start)
        {
            Playhead = Start;
        }

        OnChanged();
    }

    public void MoveEndTo(double seconds, bool parkPlayhead = true)
    {
        if (Duration <= 0)
        {
            return;
        }

        End = Math.Min(WindowEnd, Math.Max(seconds, Start + MinGap));
        if (parkPlayhead || Playhead > End)
        {
            Playhead = End;
        }

        OnChanged();
    }

    public void ClearTrim()
    {
        Start = 0;
        End = Duration;
        Zoomed = false;
        OnChanged();
    }

    /// <summary>
    /// Z zooms to a close-up where the selection fills 80% of the track, leaving 10% slack on
    /// each side. If the selection changed since the last zoom, Z zooms again on the new
    /// selection; only when zooming would not get any closer does it zoom back out.
    /// </summary>
    public void ToggleZoom()
    {
        if (Duration <= 0)
        {
            return;
        }

        double slack = (End - Start) / 8;
        double newStart = Math.Max(0, Start - slack);
        double newEnd = Math.Min(Duration, End + slack);
        if (Zoomed && newStart == ViewStart && newEnd == ViewEnd)
        {
            Zoomed = false;
        }
        else
        {
            ViewStart = newStart;
            ViewEnd = newEnd;
            Zoomed = true;
        }

        OnChanged();
    }

    public double XForTime(double time, double trackX, double trackWidth) =>
        Duration <= 0 ? trackX : trackX + (time - WindowStart) / WindowLength * trackWidth;

    public double TimeForX(double x, double trackX, double trackWidth)
    {
        if (trackWidth <= 0 || Duration <= 0)
        {
            return 0;
        }

        double fraction = Math.Clamp((x - trackX) / trackWidth, 0, 1);
        return WindowStart + fraction * WindowLength;
    }

    private void OnChanged() => Changed?.Invoke(this, EventArgs.Empty);
}
