namespace Omacut.Core;

/// <summary>
/// Generates the filmstrip: evenly spaced JPEG frames over a time window, a few ffmpeg
/// children at a time, delivered strictly in order so the strip fills left to right.
/// </summary>
public static class ThumbnailStrip
{
    public const int DefaultCount = 12;
    private const int MaxParallelJobs = 4;

    public static async Task GenerateAsync(
        FfmpegTools tools,
        string path,
        double start,
        double length,
        int count,
        int height,
        Action<int, byte[]?> onReady,
        CancellationToken cancellationToken)
    {
        if (count <= 0 || length <= 0 || !tools.HasFfmpeg)
        {
            return;
        }

        int maxJobs = Math.Min(count, Math.Min(MaxParallelJobs, Math.Max(1, Environment.ProcessorCount)));
        var jobs = new Queue<Task<byte[]?>>();
        int next = 0;

        void StartNext()
        {
            // Sample the middle of each slot, like omacut.
            double time = start + length * (next + 0.5) / count;
            next++;
            jobs.Enqueue(GrabJpegAsync(tools, path, time, height, cancellationToken));
        }

        while (next < count && jobs.Count < maxJobs)
        {
            StartNext();
        }

        int emitted = 0;
        while (jobs.Count > 0)
        {
            byte[]? image;
            try
            {
                image = await jobs.Dequeue().ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // Let the remaining children be killed by the same token.
                foreach (Task<byte[]?> pending in jobs)
                {
                    _ = pending.ContinueWith(static t => _ = t.Exception, TaskScheduler.Default);
                }

                throw;
            }

            cancellationToken.ThrowIfCancellationRequested();
            onReady(emitted++, image);

            if (next < count)
            {
                StartNext();
            }
        }
    }

    public static async Task<byte[]?> GrabJpegAsync(FfmpegTools tools, string path, double time, int height, CancellationToken cancellationToken)
    {
        ToolResult result = await FfmpegTools.RunAsync(
            tools.FfmpegPath,
            [
                "-hide_banner", "-loglevel", "error",
                "-ss", TimeFormat.Ffmpeg(time),
                "-i", path,
                "-frames:v", "1",
                "-vf", $"scale=-2:{height}",
                "-f", "image2pipe", "-vcodec", "mjpeg", "-q:v", "4",
                "pipe:1",
            ],
            TimeSpan.FromSeconds(30),
            cancellationToken).ConfigureAwait(false);

        return result.Succeeded && result.StandardOutput.Length > 0 ? result.StandardOutput : null;
    }
}
