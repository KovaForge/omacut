namespace Omacut.Core;

/// <summary>
/// Which encoders the local ffmpeg build offers, so exports pick a working codec
/// (distro and Windows builds differ) and unavailable formats can be hidden.
/// </summary>
public sealed class EncoderSupport
{
    private readonly HashSet<string> _encoders;

    internal EncoderSupport(IEnumerable<string> encoders)
    {
        _encoders = new HashSet<string>(encoders, StringComparer.Ordinal);
    }

    /// <summary>Assumes a typical full ffmpeg build; used when probing fails.</summary>
    public static EncoderSupport Assumed { get; } = new(["libx264", "aac", "libvpx-vp9", "libopus", "gif"]);

    public static async Task<EncoderSupport> ProbeAsync(FfmpegTools tools, CancellationToken cancellationToken = default)
    {
        if (!tools.HasFfmpeg)
        {
            return Assumed;
        }

        try
        {
            ToolResult result = await FfmpegTools.RunAsync(
                tools.FfmpegPath, ["-hide_banner", "-encoders"], TimeSpan.FromSeconds(10), cancellationToken).ConfigureAwait(false);
            return result.Succeeded ? Parse(System.Text.Encoding.UTF8.GetString(result.StandardOutput)) : Assumed;
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return Assumed;
        }
    }

    internal static EncoderSupport Parse(string listing)
    {
        // Lines look like " V....D libx264              libx264 H.264 ...".
        var names = new List<string>();
        foreach (string raw in listing.Split('\n'))
        {
            string line = raw.Trim();
            string[] parts = line.Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 2 && parts[0].Length == 6 && "VAS".Contains(parts[0][0]) && parts[1] != "=")
            {
                names.Add(parts[1]);
            }
        }

        return new EncoderSupport(names);
    }

    public bool Has(string encoder) => _encoders.Contains(encoder);

    public string? VideoEncoder(ExportFormat format) => format switch
    {
        ExportFormat.Mp4 => First("libx264", "libopenh264", "h264_mf", "mpeg4"),
        ExportFormat.WebM => First("libvpx-vp9", "libvpx"),
        ExportFormat.Gif => First("gif"),
        _ => null,
    };

    public string? AudioEncoder(ExportFormat format) => format switch
    {
        ExportFormat.Mp4 => First("aac", "libfdk_aac"),
        ExportFormat.WebM => First("libopus", "libvorbis"),
        _ => null,
    };

    public bool Supports(ExportFormat format) => VideoEncoder(format) != null;

    private string? First(params string[] candidates) => candidates.FirstOrDefault(_encoders.Contains);
}
