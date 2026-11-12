using System.Globalization;
using System.Text;

namespace Omacut.Core;

/// <summary>
/// Presentation timestamps of a video's frames, from ffprobe packet pts.
/// Mirrors omacut's <c>ffmpeg::frameTimes</c>.
/// </summary>
public static class FrameTimes
{
    private static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(15);

    public static double[] Parse(string text, double start)
    {
        var times = new List<double>();
        foreach (string raw in text.Split('\n'))
        {
            string field = raw.Split(',')[0].Trim();
            if (double.TryParse(field, NumberStyles.Float, CultureInfo.InvariantCulture, out double time) && double.IsFinite(time))
            {
                times.Add(time - start);
            }
        }

        times.Sort();
        if (times.Count < 2)
        {
            return times.ToArray();
        }

        var unique = new List<double>(times.Count) { times[0] };
        for (int i = 1; i < times.Count; i++)
        {
            if (times[i] != unique[^1])
            {
                unique.Add(times[i]);
            }
        }

        return unique.ToArray();
    }

    /// <summary>Null when ffprobe is missing, the file is gone, or it returns no frame times.</summary>
    public static async Task<double[]?> ReadAsync(
        FfmpegTools tools,
        string path,
        double start,
        CancellationToken cancellationToken = default)
    {
        if (!tools.HasFfprobe || !File.Exists(path))
        {
            return null;
        }

        ToolResult result = await FfmpegTools.RunAsync(
            tools.FfprobePath,
            ["-v", "error", "-select_streams", "V:0", "-show_entries", "packet=pts_time", "-of", "csv=p=0", path],
            ProbeTimeout,
            cancellationToken).ConfigureAwait(false);
        if (!result.Succeeded)
        {
            return null;
        }

        double[] times = Parse(Encoding.UTF8.GetString(result.StandardOutput), start);
        return times.Length == 0 ? null : times;
    }
}
