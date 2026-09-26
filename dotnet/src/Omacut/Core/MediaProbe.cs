using System.Globalization;
using System.Text.Json;

namespace Omacut.Core;

/// <summary>What omacut needs to know about a video before editing it.</summary>
public sealed record MediaInfo(
    string Path,
    double Duration,
    int Width,
    int Height,
    double FrameRate,
    bool HasAudio)
{
    /// <summary>Length of one frame in seconds (falls back to 30 fps when unknown).</summary>
    public double FrameDuration => 1.0 / (FrameRate > 0 ? FrameRate : 30.0);
}

public sealed class MediaProbeException(string message) : Exception(message);

public static class MediaProbe
{
    // Upper bound on a probe so a stalled network mount can't hang the editor.
    private static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(15);

    public static async Task<MediaInfo> ProbeAsync(FfmpegTools tools, string path, CancellationToken cancellationToken = default)
    {
        if (!tools.HasFfprobe)
        {
            throw new MediaProbeException("`ffprobe` was not found. Install ffmpeg.");
        }

        if (!File.Exists(path))
        {
            throw new MediaProbeException("The file does not exist.");
        }

        ToolResult result = await FfmpegTools.RunAsync(
            tools.FfprobePath,
            ["-v", "error", "-print_format", "json", "-show_format", "-show_streams", path],
            ProbeTimeout,
            cancellationToken).ConfigureAwait(false);

        if (result.TimedOut)
        {
            throw new MediaProbeException("ffprobe timed out reading this file.");
        }

        if (!result.Succeeded)
        {
            string error = result.StandardError.Trim();
            throw new MediaProbeException(error.Length > 0 ? error : "ffprobe failed.");
        }

        return Parse(path, result.StandardOutput);
    }

    internal static MediaInfo Parse(string path, ReadOnlySpan<byte> json)
    {
        using JsonDocument document = JsonDocument.Parse(json.ToArray());
        JsonElement root = document.RootElement;

        JsonElement? video = null;
        bool hasAudio = false;
        if (root.TryGetProperty("streams", out JsonElement streams))
        {
            foreach (JsonElement stream in streams.EnumerateArray())
            {
                string codecType = GetString(stream, "codec_type");
                if (codecType == "video" && video == null && !IsAttachedPicture(stream))
                {
                    video = stream;
                }
                else if (codecType == "audio")
                {
                    hasAudio = true;
                }
            }
        }

        if (video is not JsonElement v)
        {
            throw new MediaProbeException("No video stream found in this file.");
        }

        int width = GetInt(v, "width");
        int height = GetInt(v, "height");
        if (IsQuarterTurn(GetRotation(v)))
        {
            (width, height) = (height, width);
        }

        // Duration can live on the stream or on the container.
        double duration = ParseDouble(GetString(v, "duration"));
        if (duration <= 0 && root.TryGetProperty("format", out JsonElement format))
        {
            duration = ParseDouble(GetString(format, "duration"));
        }

        if (duration <= 0)
        {
            throw new MediaProbeException("Could not determine the video duration.");
        }

        if (width <= 0 || height <= 0)
        {
            throw new MediaProbeException("Could not determine the video size.");
        }

        double frameRate = ParseRate(GetString(v, "avg_frame_rate"));
        if (frameRate <= 0 || frameRate > 1000)
        {
            frameRate = ParseRate(GetString(v, "r_frame_rate"));
        }

        return new MediaInfo(path, duration, width, height, frameRate is > 0 and <= 1000 ? frameRate : 30.0, hasAudio);
    }

    private static bool IsAttachedPicture(JsonElement stream) =>
        stream.TryGetProperty("disposition", out JsonElement disposition)
        && disposition.TryGetProperty("attached_pic", out JsonElement attached)
        && attached.ValueKind == JsonValueKind.Number
        && attached.GetInt32() == 1;

    private static double GetRotation(JsonElement stream)
    {
        if (stream.TryGetProperty("side_data_list", out JsonElement sideData) && sideData.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement entry in sideData.EnumerateArray())
            {
                if (entry.TryGetProperty("rotation", out JsonElement rotation) && rotation.ValueKind == JsonValueKind.Number)
                {
                    return rotation.GetDouble();
                }
            }
        }

        if (stream.TryGetProperty("tags", out JsonElement tags))
        {
            return ParseDouble(GetString(tags, "rotate"));
        }

        return 0;
    }

    internal static bool IsQuarterTurn(double rotation)
    {
        double normalized = ((Math.Round(rotation) % 360) + 360) % 360;
        return normalized is 90 or 270;
    }

    internal static double ParseRate(string rate)
    {
        if (string.IsNullOrEmpty(rate))
        {
            return 0;
        }

        int slash = rate.IndexOf('/');
        if (slash < 0)
        {
            return ParseDouble(rate);
        }

        double numerator = ParseDouble(rate[..slash]);
        double denominator = ParseDouble(rate[(slash + 1)..]);
        return denominator > 0 ? numerator / denominator : 0;
    }

    private static double ParseDouble(string value) =>
        double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double parsed) && double.IsFinite(parsed) ? parsed : 0;

    private static string GetString(JsonElement element, string name) =>
        element.TryGetProperty(name, out JsonElement value)
            ? value.ValueKind switch
            {
                JsonValueKind.String => value.GetString() ?? string.Empty,
                JsonValueKind.Number => value.GetRawText(),
                _ => string.Empty,
            }
            : string.Empty;

    private static int GetInt(JsonElement element, string name) =>
        element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int parsed) ? parsed : 0;
}
