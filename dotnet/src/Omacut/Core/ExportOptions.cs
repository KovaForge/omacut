namespace Omacut.Core;

public enum ExportFormat
{
    Mp4,
    WebM,
    Gif,
}

/// <summary>A crop rectangle in displayed (rotation-applied) source pixels.</summary>
public readonly record struct CropRect(int X, int Y, int Width, int Height)
{
    public bool IsEmpty => Width <= 0 || Height <= 0;

    /// <summary>
    /// Clamps to the frame and rounds to even values, which yuv420p encoders require.
    /// </summary>
    public CropRect Normalize(int frameWidth, int frameHeight)
    {
        int x = Math.Clamp(X, 0, Math.Max(frameWidth - 2, 0)) & ~1;
        int y = Math.Clamp(Y, 0, Math.Max(frameHeight - 2, 0)) & ~1;
        int width = Math.Clamp(Width, 2, frameWidth - x) & ~1;
        int height = Math.Clamp(Height, 2, frameHeight - y) & ~1;
        return new CropRect(x, y, Math.Max(width, 2), Math.Max(height, 2));
    }

    public bool CoversFrame(int frameWidth, int frameHeight) =>
        X <= 0 && Y <= 0 && Width >= frameWidth - 1 && Height >= frameHeight - 1;
}

/// <summary>
/// An image the host wants burned into exports (XerahS renders its text watermark to PNG).
/// Position fractions place the image inside the frame: 0 = left/top, 1 = right/bottom.
/// </summary>
public sealed record WatermarkOverlay(string ImagePath, double Opacity = 0.8, double PositionX = 0.95, double PositionY = 0.95)
{
    public bool IsUsable => !string.IsNullOrWhiteSpace(ImagePath) && File.Exists(ImagePath) && Opacity > 0;
}

public sealed record ExportOptions
{
    public required string SourcePath { get; init; }
    public required double Start { get; init; }
    public required double End { get; init; }
    public ExportFormat Format { get; init; } = ExportFormat.Mp4;

    /// <summary>Cap the shorter side to this many pixels; 0 keeps the original size. Never upscales.</summary>
    public int ShortSide { get; init; }

    public bool IncludeAudio { get; init; } = true;
    public CropRect? Crop { get; init; }
    public WatermarkOverlay? Watermark { get; init; }

    /// <summary>Frame rate for GIF exports.</summary>
    public int GifFrameRate { get; init; } = 15;

    public double Length => Math.Max(End - Start, 0);
}

public static class ExportFormats
{
    public static string Extension(ExportFormat format) => format switch
    {
        ExportFormat.WebM => ".webm",
        ExportFormat.Gif => ".gif",
        _ => ".mp4",
    };

    public static string DisplayName(ExportFormat format) => format switch
    {
        ExportFormat.WebM => "WebM",
        ExportFormat.Gif => "GIF",
        _ => "MP4",
    };

    /// <summary>
    /// The downscale sizes worth offering: only ones strictly below the frame's shorter
    /// side, so exports never upscale. GIFs get smaller steps because they are huge.
    /// </summary>
    public static IReadOnlyList<int> ShortSides(ExportFormat format, int width, int height)
    {
        int shortSide = Math.Min(width, height);
        int[] candidates = format == ExportFormat.Gif ? [720, 480, 360, 240] : [1080, 720, 480];
        return candidates.Where(candidate => shortSide > candidate).ToArray();
    }

    /// <summary>"name_trimmed.ext" next to the source, like omacut.</summary>
    public static string SuggestedPath(string sourcePath, ExportFormat format)
    {
        string directory = Path.GetDirectoryName(sourcePath) ?? string.Empty;
        return Path.Combine(directory, Path.GetFileNameWithoutExtension(sourcePath) + "_trimmed" + Extension(format));
    }

    /// <summary>Forces the extension that matches the format.</summary>
    public static string WithExtension(string path, ExportFormat format)
    {
        string extension = Extension(format);
        return string.Equals(Path.GetExtension(path), extension, StringComparison.OrdinalIgnoreCase)
            ? path
            : Path.ChangeExtension(path, extension);
    }
}
