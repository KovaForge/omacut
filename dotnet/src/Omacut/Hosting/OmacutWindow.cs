using Avalonia;
using Avalonia.Controls;
using Omacut.Controls;

namespace Omacut.Hosting;

/// <summary>A window hosting the editor; closing it respects the unexported-edit confirmation.</summary>
public sealed class OmacutWindow : Window
{
    private readonly OmacutEditorOptions _options;
    private bool _closeConfirmed;

    public OmacutWindow(OmacutEditorOptions? options = null)
    {
        _options = options ?? new OmacutEditorOptions();
        Editor = new EditorView(_options);
        Content = Editor;
        Title = _options.WindowTitle;
        Width = 960;
        Height = 680;
        MinWidth = 640;
        MinHeight = 460;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = Avalonia.Media.Brushes.Black;

        Editor.TitleChanged += (_, name) => Title = string.IsNullOrEmpty(name) ? _options.WindowTitle : $"{_options.WindowTitle} — {name}";
        Editor.CloseRequested += (_, _) =>
        {
            _closeConfirmed = true;
            Close();
        };

        Opened += async (_, _) =>
        {
            Editor.Focus();
            if (!string.IsNullOrWhiteSpace(_options.VideoPath))
            {
                await Editor.OpenAsync(_options.VideoPath);
            }
        };
    }

    public EditorView Editor { get; }

    /// <summary>The last successful export, or null when nothing was exported.</summary>
    public string? ExportedPath => Editor.LastExportedPath;

    protected override void OnClosing(WindowClosingEventArgs e)
    {
        if (!_closeConfirmed && !Editor.RequestClose())
        {
            e.Cancel = true;
        }

        base.OnClosing(e);
    }

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);
        Editor.Dispose();
    }
}

/// <summary>One-call entry point for hosts.</summary>
public static class OmacutEditor
{
    /// <summary>
    /// Opens the editor (on the UI thread) and completes when its window closes, with the last
    /// exported path or null. With an owner the window is modal to it.
    /// </summary>
    public static async Task<string?> ShowAsync(OmacutEditorOptions options, Window? owner = null)
    {
        var window = new OmacutWindow(options);
        if (owner != null)
        {
            await window.ShowDialog(owner);
        }
        else
        {
            var closed = new TaskCompletionSource();
            window.Closed += (_, _) => closed.TrySetResult();
            window.Show();
            await closed.Task;
        }

        return window.ExportedPath;
    }
}
