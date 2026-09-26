using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;
using Omacut.Hosting;

namespace Omacut.App;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args) =>
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args, ShutdownMode.OnMainWindowClose);

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<OmacutApp>().UsePlatformDetect().WithInterFont();
}

internal sealed class OmacutApp : Application
{
    public override void Initialize()
    {
        Styles.Add(new FluentTheme());
        RequestedThemeVariant = ThemeVariant.Dark;
        Name = "omacut";
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            string? video = desktop.Args?.FirstOrDefault(arg => !arg.StartsWith('-'));
            desktop.MainWindow = new OmacutWindow(new OmacutEditorOptions
            {
                VideoPath = video is null ? null : Path.GetFullPath(video),
                Log = Console.Error.WriteLine,
            });
        }

        base.OnFrameworkInitializationCompleted();
    }
}
