using NUnit.Framework;
using Omacut.Core;
using Omacut.Playback;

namespace Omacut.Tests;

/// <summary>End-to-end checks against the real ffmpeg on PATH (skipped when it is missing).</summary>
[NonParallelizable]
public class FfmpegIntegrationTests
{
    private static readonly FfmpegTools Tools = new();
    private string _dir = null!;
    private string _video = null!;

    [OneTimeSetUp]
    public async Task CreateClip()
    {
        if (!Tools.HasFfmpeg || !Tools.HasFfprobe)
        {
            Assert.Ignore("ffmpeg/ffprobe not on PATH");
        }

        _dir = Directory.CreateTempSubdirectory("omacut-it").FullName;
        _video = Path.Combine(_dir, "source clip.mp4");
        // 4 s, 25 fps, odd-free 640x360 test pattern with a silent stereo track.
        await RunFfmpeg(
            "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4",
            "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo",
            "-shortest", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", _video);
    }

    [OneTimeTearDown]
    public void Cleanup()
    {
        if (_dir != null && Directory.Exists(_dir))
        {
            Directory.Delete(_dir, recursive: true);
        }
    }

    private static async Task RunFfmpeg(params string[] args)
    {
        ToolResult result = await FfmpegTools.RunAsync(Tools.FfmpegPath, ["-y", "-hide_banner", "-loglevel", "error", .. args], TimeSpan.FromMinutes(1));
        Assert.That(result.Succeeded, Is.True, result.StandardError);
    }

    [Test]
    public async Task ProbesTheClip()
    {
        MediaInfo info = await MediaProbe.ProbeAsync(Tools, _video);
        Assert.Multiple(() =>
        {
            Assert.That((info.Width, info.Height), Is.EqualTo((640, 360)));
            Assert.That(info.Duration, Is.EqualTo(4).Within(0.1));
            Assert.That(info.FrameRate, Is.EqualTo(25).Within(0.01));
            Assert.That(info.HasAudio, Is.True);
        });
    }

    [Test]
    public void ProbeReportsUnreadableFiles()
    {
        string junk = Path.Combine(_dir, "junk.mp4");
        File.WriteAllText(junk, "not a video");
        Assert.ThrowsAsync<MediaProbeException>(() => MediaProbe.ProbeAsync(Tools, junk));
    }

    [Test]
    public async Task GeneratesTheFilmstripInOrder()
    {
        var indexes = new List<int>();
        var sizes = new List<int>();
        await ThumbnailStrip.GenerateAsync(Tools, _video, 0, 4, 6, 60, (index, jpeg) =>
        {
            indexes.Add(index);
            sizes.Add(jpeg?.Length ?? 0);
        }, CancellationToken.None);

        Assert.That(indexes, Is.EqualTo(Enumerable.Range(0, 6)));
        Assert.That(sizes, Has.All.GreaterThan(100));
    }

    [Test]
    public async Task ExportsTrimmedCroppedMp4()
    {
        MediaInfo info = await MediaProbe.ProbeAsync(Tools, _video);
        EncoderSupport encoders = await EncoderSupport.ProbeAsync(Tools);
        var service = new ExportService(Tools, encoders);
        var progress = new List<double>();
        string output = await service.ExportAsync(
            new ExportOptions { SourcePath = _video, Start = 1, End = 2.5, Crop = new CropRect(100, 50, 321, 201) },
            info,
            Path.Combine(_dir, "out.mkv"),
            new SyncProgress(progress.Add));

        Assert.That(Path.GetExtension(output), Is.EqualTo(".mp4"), "the format's extension is forced");
        MediaInfo exported = await MediaProbe.ProbeAsync(Tools, output);
        Assert.Multiple(() =>
        {
            Assert.That((exported.Width, exported.Height), Is.EqualTo((320, 200)));
            Assert.That(exported.Duration, Is.EqualTo(1.5).Within(0.1));
            Assert.That(exported.HasAudio, Is.True);
            Assert.That(progress, Is.Not.Empty);
            Assert.That(progress[^1], Is.EqualTo(1));
            Assert.That(Directory.GetFiles(_dir, "*" + ExportService.PartSuffix + "*"), Is.Empty);
        });
    }

    [Test]
    public async Task ExportsGifAndSilentWebM()
    {
        MediaInfo info = await MediaProbe.ProbeAsync(Tools, _video);
        EncoderSupport encoders = await EncoderSupport.ProbeAsync(Tools);
        var service = new ExportService(Tools, encoders);

        string gif = await service.ExportAsync(new ExportOptions { SourcePath = _video, Start = 0, End = 1, Format = ExportFormat.Gif, ShortSide = 240 }, info, Path.Combine(_dir, "anim.gif"));
        MediaInfo gifInfo = await MediaProbe.ProbeAsync(Tools, gif);
        Assert.That(gifInfo.Height, Is.EqualTo(240));

        if (!encoders.Supports(ExportFormat.WebM))
        {
            Assert.Ignore("no VP8/VP9 encoder in this ffmpeg build");
        }

        string webm = await service.ExportAsync(new ExportOptions { SourcePath = _video, Start = 0, End = 1, Format = ExportFormat.WebM, IncludeAudio = false }, info, Path.Combine(_dir, "clip.webm"));
        MediaInfo webmInfo = await MediaProbe.ProbeAsync(Tools, webm);
        Assert.That(webmInfo.HasAudio, Is.False);
    }

    [Test]
    public async Task CancelledExportKeepsTheExistingFile()
    {
        MediaInfo info = await MediaProbe.ProbeAsync(Tools, _video);
        var service = new ExportService(Tools, await EncoderSupport.ProbeAsync(Tools));
        string target = Path.Combine(_dir, "keep.mp4");
        await File.WriteAllTextAsync(target, "previous export");

        using var cancel = new CancellationTokenSource();
        cancel.Cancel();
        Assert.CatchAsync<OperationCanceledException>(() => service.ExportAsync(new ExportOptions { SourcePath = _video, Start = 0, End = 4 }, info, target, null, cancel.Token));
        Assert.That(await File.ReadAllTextAsync(target), Is.EqualTo("previous export"));
        Assert.That(Directory.GetFiles(_dir, "*" + ExportService.PartSuffix + "*"), Is.Empty);
    }

    [Test]
    public async Task RefusesToOverwriteTheSourceOrAnUnconfirmedFile()
    {
        MediaInfo info = await MediaProbe.ProbeAsync(Tools, _video);
        var service = new ExportService(Tools, await EncoderSupport.ProbeAsync(Tools));
        Assert.ThrowsAsync<ExportException>(() => service.ExportAsync(new ExportOptions { SourcePath = _video, Start = 0, End = 1 }, info, _video));

        string existing = Path.Combine(_dir, "exists.mp4");
        await File.WriteAllTextAsync(existing, "x");
        var ex = Assert.ThrowsAsync<ExportException>(() => service.ExportAsync(new ExportOptions { SourcePath = _video, Start = 0, End = 1 }, info, Path.Combine(_dir, "exists.mov")));
        Assert.That(ex!.Message, Does.Contain("already exists"));
    }

    [Test]
    public async Task EngineGrabsPausedFramesAtPreviewSize()
    {
        MediaInfo info = await MediaProbe.ProbeAsync(Tools, _video);
        using var engine = new PlaybackEngine(Tools);
        engine.Load(info);
        VideoFrame? frame = await engine.GrabFrameAsync(3.99);
        Assert.That(frame, Is.Not.Null, "grabbing near the very end must still yield a frame");
        Assert.That(frame!.Pixels.Length, Is.GreaterThanOrEqualTo(640 * 360 * 4));
        Assert.That(engine.Position, Is.EqualTo(3.99));
        engine.Return(frame);
    }

    [Test]
    public async Task EnginePlaysInRealTimeAndStopsAtTheTrimEnd()
    {
        MediaInfo info = await MediaProbe.ProbeAsync(Tools, _video);
        using var engine = new PlaybackEngine(Tools);
        engine.Muted = true;
        engine.Load(info);
        engine.Play(1.0, 2.0);

        var times = new List<double>();
        var clock = System.Diagnostics.Stopwatch.StartNew();
        bool ended = false;
        while (!ended && clock.Elapsed < TimeSpan.FromSeconds(6))
        {
            VideoFrame? frame = engine.TakeDueFrame(out ended);
            if (frame != null)
            {
                times.Add(frame.Time);
                engine.Return(frame);
            }

            await Task.Delay(8);
        }

        double wall = clock.Elapsed.TotalSeconds;
        engine.Pause();

        Assert.Multiple(() =>
        {
            Assert.That(ended, Is.True, "playback must report the trim end");
            Assert.That(times, Is.Ordered, "frames arrive in time order");
            Assert.That(times[0], Is.EqualTo(1.0).Within(0.05));
            Assert.That(times.Count, Is.GreaterThan(15), "a 1 s clip at 25 fps should present most frames");
            Assert.That(wall, Is.InRange(0.9, 2.5), "playback runs at roughly real time");
            Assert.That(engine.Position, Is.EqualTo(2.0).Within(0.1));
        });
    }

    private sealed class SyncProgress(Action<double> report) : IProgress<double>
    {
        public void Report(double value) => report(value);
    }
}
