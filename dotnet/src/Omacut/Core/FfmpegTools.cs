using System.Diagnostics;

namespace Omacut.Core;

/// <summary>
/// Locates the ffmpeg and ffprobe executables. Hosts can pin explicit paths; otherwise
/// the tools are looked up next to each other and then on PATH, like omacut does.
/// </summary>
public sealed class FfmpegTools
{
    public FfmpegTools(string? ffmpegPath = null, string? ffprobePath = null)
    {
        FfmpegPath = Resolve("ffmpeg", ffmpegPath, sibling: null);
        FfprobePath = Resolve("ffprobe", ffprobePath, sibling: FfmpegPath);
    }

    /// <summary>Resolved ffmpeg path, or empty when it could not be found.</summary>
    public string FfmpegPath { get; }

    /// <summary>Resolved ffprobe path, or empty when it could not be found.</summary>
    public string FfprobePath { get; }

    public bool HasFfmpeg => FfmpegPath.Length > 0;
    public bool HasFfprobe => FfprobePath.Length > 0;

    internal static string ExecutableName(string tool) => OperatingSystem.IsWindows() ? tool + ".exe" : tool;

    private static string Resolve(string tool, string? explicitPath, string? sibling)
    {
        if (!string.IsNullOrWhiteSpace(explicitPath) && File.Exists(explicitPath))
        {
            return Path.GetFullPath(explicitPath);
        }

        if (!string.IsNullOrEmpty(sibling))
        {
            string candidate = Path.Combine(Path.GetDirectoryName(sibling) ?? string.Empty, ExecutableName(tool));
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        return FindOnPath(tool) ?? string.Empty;
    }

    internal static string? FindOnPath(string tool)
    {
        string name = ExecutableName(tool);
        string? path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(path))
        {
            return null;
        }

        foreach (string dir in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                string candidate = Path.Combine(dir.Trim('"'), name);
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
            catch (ArgumentException)
            {
                // Malformed PATH entries are skipped, the same as a shell would.
            }
        }

        return null;
    }

    internal static Process StartProcess(string fileName, IEnumerable<string> arguments, bool redirectStdin = false)
    {
        var startInfo = new ProcessStartInfo(fileName)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = redirectStdin,
        };

        foreach (string argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        return Process.Start(startInfo) ?? throw new InvalidOperationException($"Could not start {Path.GetFileName(fileName)}.");
    }

    internal static void KillQuietly(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch (InvalidOperationException)
        {
            // Already exited between the check and the kill.
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // Access denied or exiting; nothing more to do.
        }
    }

    /// <summary>
    /// Runs a tool to completion, capturing stdout as bytes and stderr as text. Both
    /// streams drain concurrently so a chatty child can never deadlock on a full pipe.
    /// Cancellation and the timeout kill the whole process tree.
    /// </summary>
    internal static async Task<ToolResult> RunAsync(
        string fileName,
        IEnumerable<string> arguments,
        TimeSpan? timeout = null,
        CancellationToken cancellationToken = default)
    {
        using Process process = StartProcess(fileName, arguments);
        using var timeoutSource = timeout.HasValue ? new CancellationTokenSource(timeout.Value) : new CancellationTokenSource();
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, timeoutSource.Token);

        using var stdout = new MemoryStream();
        Task copyOut = process.StandardOutput.BaseStream.CopyToAsync(stdout, CancellationToken.None);
        Task<string> readErr = process.StandardError.ReadToEndAsync(CancellationToken.None);

        try
        {
            await process.WaitForExitAsync(linked.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            KillQuietly(process);
            await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
            await Task.WhenAll(copyOut, readErr).ConfigureAwait(false);
            if (cancellationToken.IsCancellationRequested)
            {
                throw;
            }

            return new ToolResult(-1, stdout.ToArray(), readErr.Result, TimedOut: true);
        }

        await Task.WhenAll(copyOut, readErr).ConfigureAwait(false);
        return new ToolResult(process.ExitCode, stdout.ToArray(), readErr.Result, TimedOut: false);
    }
}

internal readonly record struct ToolResult(int ExitCode, byte[] StandardOutput, string StandardError, bool TimedOut)
{
    public bool Succeeded => !TimedOut && ExitCode == 0;
}
