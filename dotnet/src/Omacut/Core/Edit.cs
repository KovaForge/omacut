namespace Omacut.Core;

/// <summary>
/// One stretch of a loaded video. In and out are seconds in that source.
/// Mirrors omacut's edit::Clip.
/// </summary>
public readonly record struct Clip(int Source, double In, double Out)
{
    public double Length => Out - In;
}

/// <summary>
/// What an export encodes: ordered clips, neighbours joined when one carries
/// on from the other in the same source. Mirrors omacut's edit namespace.
/// </summary>
public static class Edit
{
    public const double MinimumClip = 0.1;

    /// <summary>
    /// A millisecond covers the float noise of a split. A larger gap is a cut.
    /// </summary>
    public const double ContinueTolerance = 0.001;

    public static double Duration(IReadOnlyList<Clip> clips)
    {
        double total = 0;
        foreach (Clip clip in clips)
        {
            total += clip.Length;
        }

        return total;
    }

    /// <summary>
    /// True when <paramref name="next"/> picks up where <paramref name="clip"/> left off
    /// in the same source. Join still requires an exact edge; this tolerance is only for
    /// playback and for export, where a split's float noise is not a cut.
    /// </summary>
    public static bool Continues(Clip clip, Clip next) =>
        clip.Source == next.Source && Math.Abs(clip.Out - next.In) <= ContinueTolerance;

    public static List<Clip> Merged(IReadOnlyList<Clip> clips)
    {
        var result = new List<Clip>(clips.Count);
        foreach (Clip clip in clips)
        {
            if (result.Count > 0 && Continues(result[^1], clip))
            {
                Clip last = result[^1];
                result[^1] = last with { Out = clip.Out };
            }
            else
            {
                result.Add(clip);
            }
        }

        return result;
    }

    /// <summary>Nothing cut, added or reordered: all of the first source, as it is.</summary>
    public static bool Untouched(IReadOnlyList<Clip> clips, double firstSourceDuration) =>
        Merged(clips).SequenceEqual(new[] { new Clip(0, 0, firstSourceDuration) });
}
