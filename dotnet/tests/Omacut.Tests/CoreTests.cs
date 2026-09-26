using System.Text;
using NUnit.Framework;
using Omacut.Core;

namespace Omacut.Tests;

public class TimeFormatTests
{
    [TestCase(0, "00:00.00")]
    [TestCase(5.25, "00:05.25")]
    [TestCase(59.999, "01:00.00")]
    [TestCase(61.5, "01:01.50")]
    [TestCase(3725.01, "62:05.01")]
    [TestCase(-3, "00:00.00")]
    [TestCase(double.NaN, "00:00.00")]
    public void FormatsLikeOmacut(double seconds, string expected) =>
        Assert.That(TimeFormat.Format(seconds), Is.EqualTo(expected));
}

public class OmarchyThemeTests
{
    private string _dir = null!;

    [SetUp]
    public void SetUp() => _dir = Directory.CreateTempSubdirectory("omacut-theme").FullName;

    [TearDown]
    public void TearDown() => Directory.Delete(_dir, recursive: true);

    private string Write(string content)
    {
        string path = Path.Combine(_dir, "colors.toml");
        File.WriteAllText(path, content);
        return path;
    }

    [Test]
    public void ReadsQuotedAccent() =>
        Assert.That(OmarchyTheme.AccentFromColorsFile(Write("# theme\nforeground = \"#fff\"\naccent = \"#7aa2f7\"\n"), "#FFD60A"), Is.EqualTo("#7aa2f7"));

    [Test]
    public void ReadsSingleQuotedAndBareAccent()
    {
        Assert.That(OmarchyTheme.AccentFromColorsFile(Write("accent='#e0af68'"), "#FFD60A"), Is.EqualTo("#e0af68"));
        Assert.That(OmarchyTheme.AccentFromColorsFile(Write("accent = #abc"), "#FFD60A"), Is.EqualTo("#abc"));
    }

    [Test]
    public void FallsBackWhenMissingOrInvalid()
    {
        Assert.That(OmarchyTheme.AccentFromColorsFile(Path.Combine(_dir, "none.toml"), "#FFD60A"), Is.EqualTo("#FFD60A"));
        Assert.That(OmarchyTheme.AccentFromColorsFile(Write("accent = \"not-a-color\""), "#FFD60A"), Is.EqualTo("#FFD60A"));
        Assert.That(OmarchyTheme.AccentFromColorsFile(Write("background = \"#000\""), "#FFD60A"), Is.EqualTo("#FFD60A"));
    }

    [Test]
    public void ChoosesLegibleForeground()
    {
        Assert.That(OmarchyTheme.ForegroundFor("#FFD60A"), Is.EqualTo("#000000"));
        Assert.That(OmarchyTheme.ForegroundFor("#1a1b26"), Is.EqualTo("#FFFFFF"));
    }
}

public class MediaProbeParseTests
{
    private static MediaInfo Parse(string json) => MediaProbe.Parse("/v.mp4", Encoding.UTF8.GetBytes(json));

    [Test]
    public void ReadsVideoAudioAndRate()
    {
        MediaInfo info = Parse("""
            {"streams":[{"codec_type":"video","width":1920,"height":1080,"avg_frame_rate":"30000/1001","duration":"12.5"},{"codec_type":"audio"}],
             "format":{"duration":"12.6"}}
            """);
        Assert.Multiple(() =>
        {
            Assert.That(info.Width, Is.EqualTo(1920));
            Assert.That(info.Height, Is.EqualTo(1080));
            Assert.That(info.Duration, Is.EqualTo(12.5));
            Assert.That(info.FrameRate, Is.EqualTo(29.97).Within(0.01));
            Assert.That(info.HasAudio, Is.True);
        });
    }

    [Test]
    public void SwapsSizeForRotatedVideoAndUsesContainerDuration()
    {
        MediaInfo info = Parse("""
            {"streams":[{"codec_type":"video","width":1920,"height":1080,"r_frame_rate":"60/1","side_data_list":[{"rotation":-90}]}],
             "format":{"duration":"3.0"}}
            """);
        Assert.Multiple(() =>
        {
            Assert.That((info.Width, info.Height), Is.EqualTo((1080, 1920)));
            Assert.That(info.Duration, Is.EqualTo(3.0));
            Assert.That(info.FrameRate, Is.EqualTo(60));
            Assert.That(info.HasAudio, Is.False);
        });
    }

    [Test]
    public void SkipsCoverArtStreams()
    {
        MediaInfo info = Parse("""
            {"streams":[{"codec_type":"video","width":300,"height":300,"disposition":{"attached_pic":1}},
                        {"codec_type":"video","width":640,"height":480,"avg_frame_rate":"25/1"}],"format":{"duration":"1"}}
            """);
        Assert.That(info.Width, Is.EqualTo(640));
    }

    [Test]
    public void RejectsFilesWithoutVideo() =>
        Assert.Throws<MediaProbeException>(() => Parse("""{"streams":[{"codec_type":"audio"}],"format":{"duration":"1"}}"""));
}

public class ExportFormatTests
{
    [Test]
    public void OnlyOffersDownscales()
    {
        Assert.That(ExportFormats.ShortSides(ExportFormat.Mp4, 1920, 1080), Is.EqualTo(new[] { 720, 480 }));
        Assert.That(ExportFormats.ShortSides(ExportFormat.Mp4, 3840, 2160), Is.EqualTo(new[] { 1080, 720, 480 }));
        Assert.That(ExportFormats.ShortSides(ExportFormat.Mp4, 1080, 1920), Is.EqualTo(new[] { 720, 480 }));
        Assert.That(ExportFormats.ShortSides(ExportFormat.Mp4, 640, 480), Is.Empty);
        Assert.That(ExportFormats.ShortSides(ExportFormat.Gif, 1920, 1080), Is.EqualTo(new[] { 720, 480, 360, 240 }));
    }

    [Test]
    public void SuggestsTrimmedNameAndForcesExtension()
    {
        string source = Path.Combine("dir", "clip.mkv");
        Assert.That(ExportFormats.SuggestedPath(source, ExportFormat.Mp4), Is.EqualTo(Path.Combine("dir", "clip_trimmed.mp4")));
        Assert.That(ExportFormats.WithExtension("a/b.MP4", ExportFormat.Mp4), Is.EqualTo("a/b.MP4"));
        Assert.That(ExportFormats.WithExtension("a/b.mp4", ExportFormat.Gif), Is.EqualTo("a/b.gif"));
    }

    [Test]
    public void CropNormalizesToEvenValuesInsideTheFrame()
    {
        CropRect crop = new CropRect(-5, 11, 5000, 101).Normalize(1920, 1080);
        Assert.That(crop, Is.EqualTo(new CropRect(0, 10, 1920, 100)));
        Assert.That(new CropRect(0, 0, 1920, 1080).CoversFrame(1920, 1080), Is.True);
    }
}

public class ExportArgumentTests
{
    private static readonly MediaInfo Video = new("/in/clip.mp4", 20, 1920, 1080, 30, HasAudio: true);
    private static readonly EncoderSupport Full = EncoderSupport.Assumed;

    private static List<string> Build(ExportOptions options, MediaInfo? info = null, EncoderSupport? encoders = null) =>
        ExportArguments.Build(options, info ?? Video, encoders ?? Full, "/out/x.part.mp4");

    private static string After(List<string> args, string flag) => args[args.IndexOf(flag) + 1];

    [Test]
    public void TrimsWithFastAccurateSeekAndKeepsAudio()
    {
        List<string> args = Build(new ExportOptions { SourcePath = Video.Path, Start = 2.5, End = 7.25 });
        Assert.Multiple(() =>
        {
            Assert.That(After(args, "-ss"), Is.EqualTo("2.500"));
            Assert.That(args.IndexOf("-ss"), Is.LessThan(args.IndexOf("-i")));
            Assert.That(After(args, "-t"), Is.EqualTo("4.750"));
            Assert.That(After(args, "-c:v"), Is.EqualTo("libx264"));
            Assert.That(After(args, "-c:a"), Is.EqualTo("aac"));
            Assert.That(args, Does.Contain("+faststart"));
            Assert.That(args, Does.Contain("0:a:0?"));
            Assert.That(args, Does.Not.Contain("-filter_complex"));
            Assert.That(args[^1], Is.EqualTo("/out/x.part.mp4"));
        });
    }

    [Test]
    public void CropsThenCapsTheShortSide()
    {
        List<string> args = Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 5, Crop = new CropRect(101, 51, 1601, 901), ShortSide = 720 });
        Assert.That(After(args, "-filter_complex"), Is.EqualTo("[0:v]crop=1600:900:100:50,scale='if(gt(iw,ih),-2,720)':'if(gt(iw,ih),720,-2)':flags=lanczos[vout]"));
        Assert.That(After(args, "-map"), Is.EqualTo("[vout]"));
    }

    [Test]
    public void DoesNotUpscaleSmallCrops()
    {
        List<string> args = Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 5, Crop = new CropRect(0, 0, 640, 480), ShortSide = 720 });
        Assert.That(After(args, "-filter_complex"), Is.EqualTo("[0:v]crop=640:480:0:0[vout]"));
    }

    [Test]
    public void RemovesAudioOnRequestOrWhenAbsent()
    {
        Assert.That(Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 5, IncludeAudio = false }), Does.Contain("-an"));
        Assert.That(Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 5 }, Video with { HasAudio = false }), Does.Contain("-an"));
    }

    [Test]
    public void GifUsesPaletteAndNoAudio()
    {
        List<string> args = Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 3, Format = ExportFormat.Gif, ShortSide = 480, GifFrameRate = 12 });
        string graph = After(args, "-filter_complex");
        Assert.Multiple(() =>
        {
            Assert.That(graph, Does.StartWith("[0:v]scale="));
            Assert.That(graph, Does.Contain("fps=12,split[a][b]"));
            Assert.That(graph, Does.Contain("palettegen"));
            Assert.That(graph, Does.EndWith("[vout]"));
            Assert.That(args, Does.Contain("-an"));
            Assert.That(After(args, "-f"), Is.EqualTo("gif"));
        });
    }

    [Test]
    public void WebMUsesVp9AndOpus()
    {
        List<string> args = Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 3, Format = ExportFormat.WebM });
        Assert.That(After(args, "-c:v"), Is.EqualTo("libvpx-vp9"));
        Assert.That(After(args, "-c:a"), Is.EqualTo("libopus"));
    }

    [Test]
    public void FallsBackToAvailableEncoders()
    {
        EncoderSupport limited = EncoderSupport.Parse(" V....D mpeg4                MPEG-4 part 2\n A....D aac                  AAC\n V....D libvpx               libvpx VP8\n A....D libvorbis            libvorbis\n");
        Assert.That(After(Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 3 }, encoders: limited), "-c:v"), Is.EqualTo("mpeg4"));
        List<string> webm = Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 3, Format = ExportFormat.WebM }, encoders: limited);
        Assert.That(After(webm, "-c:v"), Is.EqualTo("libvpx"));
        Assert.That(After(webm, "-c:a"), Is.EqualTo("libvorbis"));
        Assert.That(limited.Supports(ExportFormat.Gif), Is.False);
        Assert.Throws<InvalidOperationException>(() => Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 3, Format = ExportFormat.Gif }, encoders: limited));
    }

    [Test]
    public void BurnsInTheWatermarkAfterScaling()
    {
        string image = Path.GetTempFileName();
        try
        {
            List<string> args = Build(new ExportOptions
            {
                SourcePath = Video.Path, Start = 0, End = 3, ShortSide = 720,
                Watermark = new WatermarkOverlay(image, Opacity: 0.5, PositionX: 1, PositionY: 0),
            });
            Assert.That(args.Count(a => a == "-i"), Is.EqualTo(2));
            Assert.That(args.IndexOf("-t"), Is.GreaterThan(args.LastIndexOf("-i")), "-t must be an output option");
            string graph = After(args, "-filter_complex");
            Assert.That(graph, Does.Contain("[main];[1:v]format=rgba,colorchannelmixer=aa=0.5[wm];[main][wm]overlay=x='(W-w)*1':y='(H-h)*0'"));
        }
        finally
        {
            File.Delete(image);
        }
    }

    [Test]
    public void EvensOddSourceSizesForYuv420()
    {
        List<string> args = Build(new ExportOptions { SourcePath = Video.Path, Start = 0, End = 3 }, Video with { Width = 1365, Height = 767 });
        Assert.That(After(args, "-filter_complex"), Is.EqualTo("[0:v]scale=trunc(iw/2)*2:trunc(ih/2)*2[vout]"));
    }

    [Test]
    public void ParsesProgress()
    {
        Assert.That(ExportService.TryParseProgress("out_time_us=2500000", 5, out double fraction), Is.True);
        Assert.That(fraction, Is.EqualTo(0.5));
        Assert.That(ExportService.TryParseProgress("out_time_us=N/A", 5, out _), Is.False);
        Assert.That(ExportService.TryParseProgress("frame=10", 5, out _), Is.False);
    }
}

public class TrimModelTests
{
    [Test]
    public void EdgesKeepMinimumGapAndParkThePlayhead()
    {
        var model = new TrimModel();
        model.Reset(10);
        model.MoveStartTo(9.99);
        Assert.That(model.Start, Is.EqualTo(9.9).Within(1e-9));
        Assert.That(model.Playhead, Is.EqualTo(model.Start));
        model.MoveEndTo(0);
        Assert.That(model.End, Is.EqualTo(10).Within(1e-9));
        model.MoveStartTo(2);
        model.MoveEndTo(5);
        Assert.That((model.Start, model.End, model.Playhead), Is.EqualTo((2.0, 5.0, 5.0)));
        Assert.That(model.IsTrimmed, Is.True);
    }

    [Test]
    public void PlayheadStaysInsideTheTrim()
    {
        var model = new TrimModel();
        model.Reset(10);
        model.MoveStartTo(2);
        model.MoveEndTo(6);
        Assert.That(model.SeekBy(-5), Is.EqualTo(2));
        Assert.That(model.SeekBy(100), Is.EqualTo(6));
    }

    [Test]
    public void ZoomFramesTheSelectionAndTogglesBack()
    {
        var model = new TrimModel();
        model.Reset(100);
        model.MoveStartTo(40);
        model.MoveEndTo(56);
        model.ToggleZoom();
        Assert.That((model.Zoomed, model.WindowStart, model.WindowEnd), Is.EqualTo((true, 38.0, 58.0)));

        // While zoomed, edges stop at the window instead of the video bounds.
        model.MoveStartTo(0);
        Assert.That(model.Start, Is.EqualTo(38));

        model.ToggleZoom(); // selection changed: zoom again on the new selection
        Assert.That(model.Zoomed, Is.True);
        model.ToggleZoom(); // no closer: zoom out
        Assert.That((model.Zoomed, model.WindowStart, model.WindowEnd), Is.EqualTo((false, 0.0, 100.0)));
    }

    [Test]
    public void MapsTimesToTrackPositions()
    {
        var model = new TrimModel();
        model.Reset(10);
        Assert.That(model.XForTime(5, 14, 200), Is.EqualTo(114));
        Assert.That(model.TimeForX(114, 14, 200), Is.EqualTo(5));
        Assert.That(model.TimeForX(-50, 14, 200), Is.EqualTo(0));
    }
}
