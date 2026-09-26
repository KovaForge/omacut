using System.Globalization;

namespace Omacut.Core;

public static class TimeFormat
{
    /// <summary>
    /// mm:ss.cc. Rounds to centiseconds first so boundary values render correctly
    /// (59.999 becomes "01:00.00", not "00:60.00"). Hours roll into minutes like omacut.
    /// </summary>
    public static string Format(double seconds)
    {
        if (double.IsNaN(seconds) || seconds < 0)
        {
            seconds = 0;
        }

        long centiseconds = (long)Math.Round(seconds * 100, MidpointRounding.AwayFromZero);
        long minutes = centiseconds / 6000;
        double secs = (centiseconds - minutes * 6000) / 100.0;
        return minutes.ToString("00", CultureInfo.InvariantCulture) + ":" + secs.ToString("00.00", CultureInfo.InvariantCulture);
    }

    /// <summary>Seconds formatted for ffmpeg arguments (invariant culture, millisecond precision).</summary>
    internal static string Ffmpeg(double seconds) => Math.Max(seconds, 0).ToString("0.000", CultureInfo.InvariantCulture);
}
