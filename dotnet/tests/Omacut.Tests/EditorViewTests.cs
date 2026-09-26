using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.NUnit;
using Avalonia.Input;
using Avalonia.Themes.Fluent;
using Avalonia.Threading;
using NUnit.Framework;
using Omacut.Controls;
using Omacut.Core;
using Omacut.Hosting;

[assembly: AvaloniaTestApplication(typeof(Omacut.Tests.HeadlessApp))]

namespace Omacut.Tests;

public sealed class HeadlessApp : Application
{
    public override void Initialize() => Styles.Add(new FluentTheme());

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<HeadlessApp>().UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = true });
}

[NonParallelizable]
public class EditorViewTests
{
    private static readonly FfmpegTools Tools = new();
    private static string? _video;

    [OneTimeSetUp]
    public async Task CreateClip()
    {
        if (!Tools.HasFfmpeg || !Tools.HasFfprobe)
        {
            Assert.Ignore("ffmpeg/ffprobe not on PATH");
        }

        _video = Path.Combine(Directory.CreateTempSubdirectory("omacut-ui").FullName, "ui.mp4");
        ToolResult result = await FfmpegTools.RunAsync(Tools.FfmpegPath,
            ["-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=6", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", _video],
            TimeSpan.FromMinutes(1));
        Assert.That(result.Succeeded, Is.True, result.StandardError);
    }

    private static async Task<(Window Window, EditorView Editor)> OpenAsync()
    {
        var window = new OmacutWindow(new OmacutEditorOptions { VideoPath = null, FollowOmarchyTheme = false, AccentColor = "#7aa2f7" });
        window.Show();
        Assert.That(await window.Editor.OpenAsync(_video!), Is.True);
        Dispatcher.UIThread.RunJobs();
        return (window, window.Editor);
    }

    private static void Press(Window window, Key key, PhysicalKey physical, string? symbol, RawInputModifiers modifiers = RawInputModifiers.None)
    {
        window.KeyPressQwerty(physical, modifiers);
        Dispatcher.UIThread.RunJobs();
    }

    [AvaloniaTest]
    public async Task OpeningResetsTheTrimAndTitlesTheWindow()
    {
        (Window window, EditorView editor) = await OpenAsync();
        Assert.Multiple(() =>
        {
            Assert.That(window.Title, Is.EqualTo("omacut — ui.mp4"));
            Assert.That(editor.Trim.Duration, Is.EqualTo(6).Within(0.1));
            Assert.That((editor.Trim.Start, editor.Trim.Playhead), Is.EqualTo((0.0, 0.0)));
            Assert.That(editor.HasUnexportedEdit, Is.False);
            Assert.That(editor.RequestClose(), Is.True, "an untouched video closes without asking");
        });
        window.Close();
    }

    [AvaloniaTest]
    public async Task KeyboardTrimsAndUnexportedEditsAskBeforeQuitting()
    {
        (Window window, EditorView editor) = await OpenAsync();

        Press(window, Key.Right, PhysicalKey.ArrowRight, null);           // playhead 1 s
        Press(window, Key.I, PhysicalKey.I, "i");                          // trim start
        Press(window, Key.Right, PhysicalKey.ArrowRight, null, RawInputModifiers.Shift); // +5 s, clamped to 6
        Press(window, Key.Left, PhysicalKey.ArrowLeft, null);              // 5 s
        Press(window, Key.O, PhysicalKey.O, "o");                          // trim end

        Assert.That((editor.Trim.Start, editor.Trim.End), Is.EqualTo((1.0, 5.0)));
        Assert.That(editor.HasUnexportedEdit, Is.True);

        Press(window, Key.Q, PhysicalKey.Q, "q");
        Assert.That(editor.IsQuitConfirmationVisible, Is.True, "Q asks first when the trim was not exported");
        Press(window, Key.Escape, PhysicalKey.Escape, null);
        Assert.That(editor.IsQuitConfirmationVisible, Is.False);

        window.Close();
        Dispatcher.UIThread.RunJobs();
        Assert.That(window.IsVisible, Is.True, "closing the window also asks");
        Assert.That(editor.IsQuitConfirmationVisible, Is.True);
    }

    [AvaloniaTest]
    public async Task ZoomFramesTheSelection()
    {
        (Window window, EditorView editor) = await OpenAsync();
        Press(window, Key.Right, PhysicalKey.ArrowRight, null);
        Press(window, Key.Right, PhysicalKey.ArrowRight, null);
        Press(window, Key.I, PhysicalKey.I, "i");
        Press(window, Key.Z, PhysicalKey.Z, "z");
        Assert.That(editor.Trim.Zoomed, Is.True);
        Assert.That(editor.Trim.WindowStart, Is.EqualTo(1.5).Within(1e-9));
        Press(window, Key.Z, PhysicalKey.Z, "z");
        Assert.That(editor.Trim.Zoomed, Is.False);
    }
}
