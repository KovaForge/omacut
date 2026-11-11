using System.Globalization;

namespace Omacut.Core;

public static class TimeFormat
{
    /// <summary>
    /// [hh:]mm:ss.cc. Rounds to centiseconds first so boundary values render correctly
    /// (59.999 becomes "01:00.00", not "00:60.00"). Hours get their own field past 60 minutes.
    /// </summary>
    public static string Format(double seconds)
    {
        if (double.IsNaN(seconds) || seconds < 0)
        {
            seconds = 0;
        }

        long centiseconds = (long)Math.Round(seconds * 100, MidpointRounding.AwayFromZero);
        long totalMinutes = centiseconds / 6000;
        long hours = totalMinutes / 60;
        long minutes = totalMinutes % 60;
        double secs = (centiseconds - totalMinutes * 6000) / 100.0;
        string mmss = minutes.ToString("00", CultureInfo.InvariantCulture) + ":" + secs.ToString("00.00", CultureInfo.InvariantCulture);
        return hours > 0 ? hours.ToString("00", CultureInfo.InvariantCulture) + ":" + mmss : mmss;
    }

    /// <summary>Seconds formatted for ffmpeg arguments (invariant culture, millisecond precision).</summary>
    internal static string Ffmpeg(double seconds) => Math.Max(seconds, 0).ToString("0.000", CultureInfo.InvariantCulture);
}
