namespace Omacut.Core;

/// <summary>
/// Picks the next presented frame the way omacut's <c>Backend::stepFrame</c> does.
/// Frame times are seconds in the source, relative to the container start, sorted and unique.
/// </summary>
public static class FrameStep
{
    /// <summary>Half a millisecond: the player's position can round a clip edge up by this much.</summary>
    public const double EdgeTolerance = 0.0005;

    /// <summary>
    /// Sequence time of the frame a step in <paramref name="direction"/> lands on.
    /// Returns false when there are no frame times to step through.
    /// </summary>
    public static bool TryTarget(
        Timeline timeline,
        IReadOnlyList<double> times,
        double t,
        int direction,
        out double sequenceTime)
    {
        sequenceTime = t;
        if (direction == 0 || timeline.Duration <= 0 || times.Count == 0)
        {
            return false;
        }

        t = Math.Clamp(t, 0, timeline.Duration);
        int index = ClipIndex(timeline, t, direction);
        if ((uint)index >= (uint)timeline.Clips.Count)
        {
            return false;
        }

        Clip clip = timeline.Clips[index];
        double clipStart = timeline.ClipStart(index);
        bool atClipEnd = t >= timeline.ClipStart(index + 1);
        double sourceTime = atClipEnd ? clip.Out : clip.In + t - clipStart;
        double target = direction < 0 ? clip.In : clip.Out;
        if (direction > 0)
        {
            foreach (double time in times)
            {
                if (time > sourceTime + EdgeTolerance && time < clip.Out)
                {
                    target = time;
                    break;
                }
            }
        }
        else
        {
            // The frame on screen, then the one before it.
            double shown = clip.In;
            foreach (double time in times)
            {
                if (time > sourceTime + EdgeTolerance || time >= clip.Out)
                {
                    break;
                }

                if (time >= clip.In)
                {
                    target = shown;
                    shown = time;
                }
            }

            // Only at the actual end. A frame just before it must not count as the end,
            // or the next press would land on that same frame again.
            if (atClipEnd)
            {
                target = shown;
            }
        }

        sequenceTime = clipStart + target - clip.In;
        return true;
    }

    /// <summary>
    /// The clip a step runs in. An edge belongs to the following clip, but stepping
    /// left from it shows the previous clip's last frame.
    /// </summary>
    public static int ClipIndex(Timeline timeline, double t, int direction)
    {
        t = Math.Clamp(t, 0, Math.Max(timeline.Duration, 0));
        int index = timeline.ClipAt(t);
        if (direction < 0 && index > 0 && t - timeline.ClipStart(index) <= EdgeTolerance)
        {
            index--;
        }

        return index;
    }
}
