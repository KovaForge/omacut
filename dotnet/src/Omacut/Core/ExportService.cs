using System.Diagnostics;
using System.Globalization;

namespace Omacut.Core;

public sealed class ExportException(string message) : Exception(message);

/// <summary>
/// Runs exports. ffmpeg encodes to a sibling temp file and the destination is replaced only
/// after success, so failed or cancelled exports never damage an existing file.
/// </summary>
public sealed class ExportService(FfmpegTools tools, EncoderSupport encoders)
{
    internal const string PartSuffix = ".omacut-part";

    /// <summary>
    /// Exports and returns the path actually written (the extension is forced to match the format).
    /// Progress reports 0..1. Throws <see cref="ExportException"/> with a user-facing message.
    /// </summary>
    public async Task<string> ExportAsync(
        ExportOptions options,
        MediaInfo info,
        string requestedPath,
        IProgress<double>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (!tools.HasFfmpeg)
        {
            throw new ExportException("`ffmpeg` was not found. Install ffmpeg.");
        }

        if (options.Length <= 0)
        {
            throw new ExportException("The selected clip has no length.");
        }

        string outputPath = Path.GetFullPath(ExportFormats.WithExtension(requestedPath, options.Format));
        if (PathsEqual(outputPath, Path.GetFullPath(options.SourcePath)))
        {
            throw new ExportException("Choose a different file: exporting over the source video is not allowed.");
        }

        // Forcing the extension can redirect the write to a file the save dialog never
        // asked about overwriting, so refuse rather than silently replace it.
        if (!PathsEqual(outputPath, Path.GetFullPath(requestedPath)) && File.Exists(outputPath))
        {
            throw new ExportException($"{Path.GetFileName(outputPath)} already exists.");
        }

        string tempPath = outputPath + PartSuffix + ExportFormats.Extension(options.Format);
        TryDelete(tempPath);

        List<string> args;
        try
        {
            args = ExportArguments.Build(options, info, encoders, tempPath);
        }
        catch (InvalidOperationException ex)
        {
            throw new ExportException(ex.Message);
        }

        try
        {
            await RunFfmpegAsync(args, options.Length, progress, cancellationToken).ConfigureAwait(false);
            File.Move(tempPath, outputPath, overwrite: true);
            progress?.Report(1);
            return outputPath;
        }
        catch
        {
            TryDelete(tempPath);
            throw;
        }
    }

    private async Task RunFfmpegAsync(List<string> args, double clipLength, IProgress<double>? progress, CancellationToken cancellationToken)
    {
        Process process;
        try
        {
            process = FfmpegTools.StartProcess(tools.FfmpegPath, args);
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            throw new ExportException("Could not start ffmpeg: " + ex.Message);
        }

        using (process)
        {
            Task<string> stderr = process.StandardError.ReadToEndAsync(CancellationToken.None);
            Task stdout = ReadProgressAsync(process.StandardOutput, clipLength, progress);

            try
            {
                await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                FfmpegTools.KillQuietly(process);
                await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
                await Task.WhenAll(stderr, stdout).ConfigureAwait(false);
                throw;
            }

            await Task.WhenAll(stderr, stdout).ConfigureAwait(false);
            if (process.ExitCode != 0)
            {
                string error = stderr.Result.Trim();
                throw new ExportException(error.Length > 0 ? LastLines(error, 4) : "ffmpeg export failed.");
            }
        }
    }

    // ffmpeg -progress writes key=value blocks as it encodes; out_time_us against the
    // clip length gives the fraction done.
    private static async Task ReadProgressAsync(StreamReader reader, double clipLength, IProgress<double>? progress)
    {
        while (await reader.ReadLineAsync().ConfigureAwait(false) is { } line)
        {
            if (progress != null && TryParseProgress(line, clipLength, out double fraction))
            {
                progress.Report(fraction);
            }
        }
    }

    internal static bool TryParseProgress(string line, double clipLength, out double fraction)
    {
        fraction = 0;
        const string key = "out_time_us=";
        if (clipLength <= 0 || !line.StartsWith(key, StringComparison.Ordinal))
        {
            return false;
        }

        if (!long.TryParse(line.AsSpan(key.Length).Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out long micros))
        {
            return false;
        }

        fraction = Math.Clamp(micros / 1e6 / clipLength, 0, 1);
        return true;
    }

    private static string LastLines(string text, int count)
    {
        string[] lines = text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return string.Join(Environment.NewLine, lines.Skip(Math.Max(0, lines.Length - count)));
    }

    private static bool PathsEqual(string a, string b) =>
        string.Equals(a, b, OperatingSystem.IsLinux() ? StringComparison.Ordinal : StringComparison.OrdinalIgnoreCase);

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
