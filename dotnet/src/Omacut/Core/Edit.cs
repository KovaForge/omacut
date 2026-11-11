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

    public static double Duration(IReadOnlyList<Clip> clips)
    {
        double total = 0;
        foreach (Clip clip in clips)
        {
            total += clip.Length;
        }

        return total;
    }

    public static List<Clip> Merged(IReadOnlyList<Clip> clips)
    {
        var result = new List<Clip>(clips.Count);
        foreach (Clip clip in clips)
        {
            if (result.Count > 0 && result[^1].Source == clip.Source && result[^1].Out == clip.In)
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
