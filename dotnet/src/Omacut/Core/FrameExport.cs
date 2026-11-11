namespace Omacut.Core;

/// <summary>Saves the frame on screen as a PNG next to the video. Mirrors omacut's save-frame naming.</summary>
public static class FrameExport
{
    public static IReadOnlyList<string> Arguments(string videoPath, double frameTime, string pngPath)
    {
        double target = Math.Max(frameTime - 0.0005, 0);
        return
        [
            "-loglevel", "error",
            "-y",
            "-ss", target.ToString("0.000000", System.Globalization.CultureInfo.InvariantCulture),
            "-i", videoPath,
            "-frames:v", "1",
            "-vf", "scale=round(iw*sar):ih,setsar=1",
            pngPath,
        ];
    }

    /// <summary>The first free "&lt;name&gt;-frame.png", then "-frame-2.png" and so on.</summary>
    public static string NextPath(string videoPath)
    {
        string? directory = Path.GetDirectoryName(videoPath);
        string stem = Path.Combine(string.IsNullOrEmpty(directory) ? "." : directory, Path.GetFileNameWithoutExtension(videoPath));
        string path = stem + "-frame.png";
        for (int n = 2; File.Exists(path) && n <= 1000; n++)
        {
            path = stem + "-frame-" + n.ToString(System.Globalization.CultureInfo.InvariantCulture) + ".png";
        }

        return path;
    }
}
