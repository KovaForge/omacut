using Omacut.Core;

namespace Omacut.Hosting;

/// <summary>How a host app opens the editor.</summary>
public sealed class OmacutEditorOptions
{
    /// <summary>Video to open immediately; null shows the "Open a video" start screen.</summary>
    public string? VideoPath { get; init; }

    /// <summary>ffmpeg executable; null looks next to <see cref="FfprobePath"/> and on PATH.</summary>
    public string? FfmpegPath { get; init; }

    /// <summary>ffprobe executable; null looks next to ffmpeg and on PATH.</summary>
    public string? FfprobePath { get; init; }

    /// <summary>Accent color (#RRGGBB). Null follows the Omarchy theme, falling back to omacut yellow.</summary>
    public string? AccentColor { get; init; }

    /// <summary>Keep following Omarchy theme switches even when <see cref="AccentColor"/> is set.</summary>
    public bool FollowOmarchyTheme { get; init; } = true;

    /// <summary>Window title prefix, e.g. "omacut" or the host app name.</summary>
    public string WindowTitle { get; init; } = "omacut";

    /// <summary>Image burned into every export (hosts render text watermarks to PNG).</summary>
    public WatermarkOverlay? Watermark { get; init; }

    /// <summary>Close the editor as soon as an export succeeds, handing the path back to the host.</summary>
    public bool CloseAfterExport { get; init; }

    /// <summary>Where exports go by default; null uses the source video's folder.</summary>
    public string? OutputDirectory { get; init; }

    /// <summary>Allow Q and Ctrl+O. Hosts editing one specific file can turn file switching off.</summary>
    public bool AllowOpeningOtherFiles { get; init; } = true;

    /// <summary>Called with the editor window before it is shown (icon, placement, owner tweaks).</summary>
    public Action<Avalonia.Controls.Window>? ConfigureWindow { get; init; }

    /// <summary>Diagnostics sink (ffmpeg failures, audio fallback).</summary>
    public Action<string>? Log { get; init; }
}
