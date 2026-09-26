using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Documents;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Omacut.Core;
using Omacut.Hosting;
using Omacut.Playback;

namespace Omacut.Controls;

/// <summary>
/// The whole omacut editor as an embeddable control: preview, trim bar, status line and the
/// help, export and quit overlays. Hosts put it in a window (see <see cref="OmacutWindow"/>).
/// </summary>
public sealed class EditorView : UserControl, IDisposable
{
    private const int ThumbCount = ThumbnailStrip.DefaultCount;
    private const int ThumbRevealMs = 70;
    private const int ThumbHeight = 90;

    private readonly OmacutEditorOptions _options;
    private readonly FfmpegTools _tools;
    private readonly PlaybackEngine _engine;
    private readonly TrimModel _trim = new();
    private readonly OmarchyTheme? _theme;
    private readonly DispatcherTimer _renderTimer;
    private readonly DispatcherTimer _thumbRevealTimer;
    private readonly DispatcherTimer _noticeTimer;
    private Task<EncoderSupport> _encoders;

    // ---- visuals ----
    private readonly Grid _layout;
    private readonly VideoSurface _surface;
    private readonly TrimBar _trimBar;
    private readonly IconButton _playButton;
    private readonly IconButton _muteButton;
    private readonly IconButton _cropButton;
    private readonly IconButton _exportButton;
    private readonly Grid _controlsRow;
    private readonly TextBlock _status;
    private readonly PillButton _openButton;
    private readonly TextBlock _openHint;
    private readonly Border _helpToggle;
    private readonly Border _helpOverlay;
    private readonly StackPanel _helpList;
    private readonly Border _quitOverlay;
    private readonly PillButton _quitCancel;
    private readonly PillButton _quitQuit;
    private readonly PillButton _quitExport;
    private readonly Border _exportOverlay;
    private readonly StackPanel _exportOptionsPanel;
    private readonly StackPanel _exportProgressPanel;
    private readonly Segmented<ExportFormat> _formatChoice = new(ExportFormat.Mp4);
    private readonly Segmented<int> _sizeChoice = new(0);
    private readonly Segmented<bool> _audioChoice = new(true);
    private readonly StackPanel _audioRow;
    private readonly TextBlock _exportPathText;
    private readonly TextBlock _exportSummary;
    private readonly TextBlock _exportError;
    private readonly PillButton _exportGo;
    private readonly PillButton _exportCancelButton;
    private readonly PillButton _exportChange;
    private readonly ProgressTrack _progress = new();
    private readonly TextBlock _progressText;
    private readonly PillButton _progressCancel;

    // ---- state ----
    private MediaInfo? _media;
    private Color _accent;
    private Color _accentForeground;
    private string _notice = string.Empty;
    private string _busyStatus = string.Empty;
    private bool _helpVisible;
    private bool _quitVisible;
    private bool _quitAfterExport;
    private bool _exporting;
    private CancellationTokenSource? _exportCancel;
    private string? _exportPath;
    private ExportFormat _lastFormat = ExportFormat.Mp4;
    private ExportState? _exported;
    private bool _resumeAfterScrub;
    private TopLevel? _topLevel;
    private bool _disposed;

    // Filmstrip: the visible strip, plus the full-length strip cached for zooming back out.
    private readonly Bitmap?[] _thumbs = new Bitmap?[ThumbCount];
    private readonly Bitmap?[] _fullThumbs = new Bitmap?[ThumbCount];
    private bool _fullThumbsComplete;
    private int _thumbsAvailable;
    private int _thumbsReady;
    private bool _thumbsDone;
    private double _thumbStart;
    private double _thumbLength;
    private int _thumbRevision;
    private CancellationTokenSource? _thumbCancel;

    private readonly record struct ExportState(double Start, double End, CropRect? Crop);

    public EditorView(OmacutEditorOptions? options = null)
    {
        _options = options ?? new OmacutEditorOptions();
        _tools = new FfmpegTools(_options.FfmpegPath, _options.FfprobePath);
        _engine = new PlaybackEngine(_tools);
        if (_engine.AudioError is { } audioError)
        {
            Log("Audio disabled: " + audioError);
        }

        _encoders = EncoderSupport.ProbeAsync(_tools);

        string initialAccent = OmarchyTheme.IsColor(_options.AccentColor) ? _options.AccentColor! : OmarchyTheme.DefaultAccent;
        if (_options.AccentColor == null || _options.FollowOmarchyTheme)
        {
            _theme = new OmarchyTheme(initialAccent);
            _theme.AccentChanged += (_, _) => Dispatcher.UIThread.Post(() => ApplyAccent(_theme.Accent));
            if (_options.AccentColor == null || File.Exists(OmarchyTheme.ColorsPath))
            {
                initialAccent = _theme.Accent;
            }
        }

        _renderTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(8), DispatcherPriority.Render, OnRenderTick);
        _thumbRevealTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(ThumbRevealMs), DispatcherPriority.Background, (_, _) => RevealNextThumb());
        _noticeTimer = new DispatcherTimer(TimeSpan.FromSeconds(5), DispatcherPriority.Background, (_, _) =>
        {
            _noticeTimer!.Stop();
            _notice = string.Empty;
            UpdateStatus();
        });

        Background = Palette.BackgroundBrush;
        Focusable = true;

        // ---- preview ----
        _surface = new VideoSurface();
        _surface.Clicked += (_, _) => TogglePlay();
        _surface.CropChanged += (_, _) => UpdateStatus();

        _openButton = new PillButton("Open a video", primary: true) { Height = 44, Padding = new Thickness(22, 0), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        _openButton.Click += (_, _) => _ = OpenWithPickerAsync();
        _openHint = new TextBlock
        {
            Text = "or drop a video here",
            Foreground = Palette.TextFaintBrush,
            FontSize = 13,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 90, 0, 0),
        };

        var preview = new Panel { Children = { _surface, _openButton, _openHint } };

        // ---- timeline row ----
        _trimBar = new TrimBar(_trim);
        _trimBar.Scrubbed += (_, seconds) => OnScrubbed(seconds);
        _trimBar.InteractionChanged += (_, active) => OnTrimInteraction(active);

        _playButton = new IconButton(IconKind.Play, "Play");
        _playButton.Click += (_, _) => TogglePlay();
        _muteButton = new IconButton(IconKind.VolumeOn, "Mute (M)");
        _muteButton.Click += (_, _) => ToggleMute();
        _cropButton = new IconButton(IconKind.Crop, "Crop (C)");
        _cropButton.Click += (_, _) => ToggleCropEdit();
        _exportButton = new IconButton(IconKind.Export, "Export (Ctrl+S)");
        _exportButton.Click += (_, _) => ShowExport();

        _controlsRow = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto,Auto,Auto"),
            ColumnSpacing = 10,
        };
        AddToGrid(_controlsRow, _playButton, 0);
        AddToGrid(_controlsRow, _trimBar, 1);
        AddToGrid(_controlsRow, _muteButton, 2);
        AddToGrid(_controlsRow, _cropButton, 3);
        AddToGrid(_controlsRow, _exportButton, 4);
        _trimBar.VerticalAlignment = VerticalAlignment.Center;
        foreach (Control button in new Control[] { _playButton, _muteButton, _cropButton, _exportButton })
        {
            button.VerticalAlignment = VerticalAlignment.Center;
        }

        // ---- status line ----
        _status = new TextBlock
        {
            FontFamily = Palette.Mono,
            FontSize = 13,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
            Foreground = Palette.TextSecondaryBrush,
        };

        _layout = new Grid
        {
            RowDefinitions = new RowDefinitions("*,Auto,26"),
            RowSpacing = 14,
        };
        AddToGrid(_layout, preview, 0, row: true);
        AddToGrid(_layout, _controlsRow, 1, row: true);
        AddToGrid(_layout, _status, 2, row: true);

        // ---- help toggle and overlay ----
        _helpToggle = new Border
        {
            Width = 24,
            Height = 24,
            CornerRadius = new CornerRadius(12),
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(10),
            Cursor = new Cursor(StandardCursorType.Hand),
            Background = Brushes.Transparent,
            Child = new TextBlock { Text = "?", FontSize = 14, FontWeight = FontWeight.SemiBold, Foreground = Palette.TextFaintBrush, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center },
        };
        ToolTip.SetTip(_helpToggle, "Keyboard shortcuts (?)");
        _helpToggle.PointerEntered += (_, _) => _helpToggle.Background = Palette.SurfaceBrush;
        _helpToggle.PointerExited += (_, _) => _helpToggle.Background = Brushes.Transparent;
        _helpToggle.PointerPressed += (_, e) =>
        {
            e.Handled = true;
            SetHelpVisible(!_helpVisible);
        };

        _helpList = new StackPanel { Spacing = 10 };
        _helpOverlay = Overlay(Card(_helpList), onScrimClick: () => SetHelpVisible(false));

        // ---- quit confirmation ----
        _quitCancel = new PillButton("Cancel");
        _quitCancel.Click += (_, _) => SetQuitVisible(false);
        _quitQuit = new PillButton("Quit");
        _quitQuit.Click += (_, _) => ForceClose();
        _quitExport = new PillButton("Export", primary: true);
        _quitExport.Click += (_, _) =>
        {
            SetQuitVisible(false);
            _quitAfterExport = true;
            ShowExport();
        };
        var quitContent = new StackPanel
        {
            Spacing = 8,
            Children =
            {
                Heading("Unexported edit"),
                new TextBlock { Text = "Your trim hasn't been exported. Quit anyway?", Foreground = Palette.TextSecondaryBrush, FontSize = 13, Margin = new Thickness(0, 0, 0, 12) },
                new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, HorizontalAlignment = HorizontalAlignment.Right, Children = { _quitCancel, _quitQuit, _quitExport } },
            },
        };
        _quitOverlay = Overlay(Card(quitContent), onScrimClick: () => SetQuitVisible(false));

        // ---- export panel ----
        _formatChoice.SelectionChanged += (_, _) => RefreshExportChoices();
        _sizeChoice.SelectionChanged += (_, _) => RefreshExportSummary();
        _audioChoice.SelectionChanged += (_, _) => RefreshExportSummary();
        _exportPathText = new TextBlock
        {
            Foreground = Palette.TextSecondaryBrush,
            FontSize = 13,
            TextTrimming = TextTrimming.LeadingCharacterEllipsis,
            VerticalAlignment = VerticalAlignment.Center,
            MaxWidth = 360,
        };
        _exportChange = new PillButton("Change…") { Height = 30 };
        _exportChange.Click += (_, _) => _ = PickExportPathAsync();
        _exportSummary = new TextBlock { Foreground = Palette.TextMutedBrush, FontSize = 12, FontFamily = Palette.Mono, TextWrapping = TextWrapping.Wrap };
        _exportError = new TextBlock { Foreground = Palette.DangerBrush, FontSize = 12, TextWrapping = TextWrapping.Wrap, MaxWidth = 460, IsVisible = false };
        _exportCancelButton = new PillButton("Cancel");
        _exportCancelButton.Click += (_, _) => HideExport();
        _exportGo = new PillButton("Export", primary: true);
        _exportGo.Click += (_, _) => _ = RunExportAsync();
        _audioRow = OptionRow("Audio", _audioChoice);

        _exportOptionsPanel = new StackPanel
        {
            Spacing = 14,
            Width = 480,
            Children =
            {
                Heading("Export"),
                OptionRow("Format", _formatChoice),
                OptionRow("Size", _sizeChoice),
                _audioRow,
                OptionRow("Save to", new DockPanel { LastChildFill = true, Children = { Dock(_exportChange, Avalonia.Controls.Dock.Right), _exportPathText } }),
                _exportSummary,
                _exportError,
                new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 6, 0, 0), Children = { _exportCancelButton, _exportGo } },
            },
        };

        _progressText = new TextBlock { Foreground = Palette.TextSecondaryBrush, FontSize = 13, FontFamily = Palette.Mono };
        _progressCancel = new PillButton("Cancel");
        _progressCancel.Click += (_, _) => _exportCancel?.Cancel();
        _exportProgressPanel = new StackPanel
        {
            Spacing = 14,
            Width = 420,
            IsVisible = false,
            Children =
            {
                Heading("Exporting"),
                _progress,
                new DockPanel { Children = { Dock(_progressCancel, Avalonia.Controls.Dock.Right), _progressText } },
            },
        };

        _exportOverlay = Overlay(Card(new Panel { Children = { _exportOptionsPanel, _exportProgressPanel } }), onScrimClick: () =>
        {
            if (!_exporting)
            {
                HideExport();
            }
        });

        Content = new Panel { Children = { _layout, _helpToggle, _helpOverlay, _quitOverlay, _exportOverlay } };

        DragDrop.SetAllowDrop(this, true);
        AddHandler(DragDrop.DragOverEvent, OnDragOver);
        AddHandler(DragDrop.DropEvent, OnDrop);

        BuildHelp();
        ApplyAccent(initialAccent);
        UpdateChrome();
    }

    /// <summary>Raised after every successful export with the written path.</summary>
    public event EventHandler<string>? Exported;

    /// <summary>Raised when the editor wants its window closed (Q, quit dialog, close-after-export).</summary>
    public event EventHandler? CloseRequested;

    /// <summary>Raised when the loaded file changes, with the file name (empty when none).</summary>
    public event EventHandler<string>? TitleChanged;

    public string? LastExportedPath { get; private set; }

    internal TrimModel Trim => _trim;
    internal bool IsQuitConfirmationVisible => _quitVisible;
    internal bool IsExportPanelVisible => _exportOverlay.IsVisible;
    public string? SourcePath => _media?.Path;

    /// <summary>True when the trim/crop differs from what was last exported.</summary>
    public bool HasUnexportedEdit =>
        _media != null && (_trim.IsTrimmed || _surface.Crop != null) && _exported != CurrentState;

    private ExportState CurrentState => new(_trim.Start, _trim.End, _surface.Crop);

    private void Log(string message) => _options.Log?.Invoke("[omacut] " + message);

    // =====================================================================================
    // Loading
    // =====================================================================================

    public async Task<bool> OpenAsync(string path)
    {
        if (_exporting)
        {
            return false;
        }

        _busyStatus = "Loading…";
        UpdateStatus();
        MediaInfo media;
        try
        {
            media = await MediaProbe.ProbeAsync(_tools, path);
        }
        catch (MediaProbeException ex)
        {
            _busyStatus = string.Empty;
            ShowNotice("Cannot open video: " + ex.Message);
            Log($"Probe failed for {path}: {ex.Message}");
            return false;
        }

        Pause();
        _media = media;
        _engine.Load(media);
        _trim.Reset(media.Duration);
        _surface.Clear();
        _surface.SetSource(media.Width, media.Height);
        _exported = null;
        _notice = string.Empty;
        _exportPath = null;
        TitleChanged?.Invoke(this, Path.GetFileName(path));

        StartThumbs(0, media.Duration, resetCache: true);
        UpdateChrome();
        RequestFrame(0);
        Focus();
        return true;
    }

    private async Task OpenWithPickerAsync()
    {
        if (_topLevel?.StorageProvider is not { } storage || _exporting)
        {
            return;
        }

        IReadOnlyList<IStorageFile> files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Open a video",
            AllowMultiple = false,
            FileTypeFilter =
            [
                new FilePickerFileType("Videos") { Patterns = ["*.mp4", "*.mkv", "*.webm", "*.mov", "*.avi", "*.m4v", "*.gif", "*.wmv", "*.flv", "*.ts"], MimeTypes = ["video/*"] },
                FilePickerFileTypes.All,
            ],
        });

        if (files.Count > 0 && files[0].TryGetLocalPath() is { } path)
        {
            await OpenAsync(path);
        }
    }

    private void OnDragOver(object? sender, DragEventArgs e)
    {
        e.DragEffects = !_exporting && _options.AllowOpeningOtherFiles && e.DataTransfer.Formats.Contains(DataFormat.File)
            ? DragDropEffects.Copy
            : DragDropEffects.None;
    }

    private void OnDrop(object? sender, DragEventArgs e)
    {
        if (_exporting || !_options.AllowOpeningOtherFiles)
        {
            return;
        }

        foreach (IDataTransferItem item in e.DataTransfer.Items)
        {
            if (item.TryGetRaw(DataFormat.File) is IStorageFile file && file.TryGetLocalPath() is { } path)
            {
                _ = OpenAsync(path);
                break;
            }
        }
    }

    // =====================================================================================
    // Playback
    // =====================================================================================

    private bool IsPlaying => _engine.IsPlaying;

    private void TogglePlay()
    {
        if (_media == null || _surface.IsEditingCrop)
        {
            return;
        }

        if (IsPlaying)
        {
            Pause();
            return;
        }

        // A finished clip rests a hair before the end; treat anything within 10 ms of the end as
        // "at the end" so play restarts from the trim start instead of instantly re-pausing.
        double from = _trim.Playhead;
        if (from < _trim.Start || from >= _trim.End - 0.01)
        {
            from = _trim.Start;
            _trim.SetPlayhead(from);
        }

        _engine.Play(from, _trim.End);
        _renderTimer.Start();
        UpdateChrome();
    }

    private void Pause()
    {
        if (!IsPlaying)
        {
            return;
        }

        double position = _engine.Pause();
        _renderTimer.Stop();
        if (!_trimBar.IsInteracting)
        {
            _trim.SetPlayhead(position);
        }

        UpdateChrome();
    }

    private void OnRenderTick(object? sender, EventArgs e)
    {
        VideoFrame? frame = _engine.TakeDueFrame(out bool ended);
        if (frame != null)
        {
            _surface.ShowFrame(frame.Pixels, _engine.FrameWidth, _engine.FrameHeight);
            _engine.Return(frame);
        }

        if (!_trimBar.IsInteracting)
        {
            _trim.SetPlayhead(_engine.Position);
            UpdateStatus();
        }

        if (ended || !IsPlaying)
        {
            // Stop at the trim end, like a clip preview.
            _engine.Pause();
            _renderTimer.Stop();
            _trim.SetPlayhead(_trim.End);
            UpdateChrome();
        }
    }

    private void MovePlayheadTo(double seconds)
    {
        _trim.SetPlayhead(seconds);
        if (IsPlaying)
        {
            _engine.Play(_trim.Playhead, _trim.End);
        }
        else
        {
            RequestFrame(_trim.Playhead);
        }

        UpdateStatus();
    }

    private void SeekBy(double delta)
    {
        if (_media == null)
        {
            return;
        }

        MovePlayheadTo(_trim.Playhead + delta);
    }

    private void StepFrame(int direction)
    {
        if (_media == null)
        {
            return;
        }

        Pause();
        MovePlayheadTo(_trim.Playhead + direction * _media.FrameDuration);
    }

    private void RequestFrame(double time)
    {
        _ = GrabAndShowAsync(time);
    }

    private async Task GrabAndShowAsync(double time)
    {
        VideoFrame? frame;
        try
        {
            frame = await _engine.GrabFrameAsync(time);
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            Log("Frame grab failed: " + ex.Message);
            return;
        }

        if (frame == null)
        {
            return;
        }

        await Dispatcher.UIThread.InvokeAsync(() =>
        {
            if (!_disposed && !IsPlaying && _media != null)
            {
                _surface.ShowFrame(frame.Pixels, _engine.FrameWidth, _engine.FrameHeight);
                UpdateChrome();
            }

            _engine.Return(frame);
        });
    }

    private void OnScrubbed(double seconds)
    {
        RequestFrame(seconds);
        UpdateStatus();
    }

    private void OnTrimInteraction(bool active)
    {
        if (active)
        {
            // Dragging takes over the picture; resume afterwards if it was playing.
            _resumeAfterScrub = IsPlaying;
            Pause();
        }
        else if (_resumeAfterScrub)
        {
            _resumeAfterScrub = false;
            TogglePlay();
        }

        UpdateStatus();
    }

    private void ToggleMute()
    {
        if (_media == null)
        {
            return;
        }

        _engine.Muted = !_engine.Muted;
        UpdateChrome();
    }

    // =====================================================================================
    // Trim, zoom and crop
    // =====================================================================================

    private void MoveTrimStartTo(double seconds)
    {
        if (_media == null)
        {
            return;
        }

        _trim.MoveStartTo(seconds);
        MovePlayheadTo(_trim.Start);
    }

    private void MoveTrimEndTo(double seconds)
    {
        if (_media == null)
        {
            return;
        }

        _trim.MoveEndTo(seconds);
        MovePlayheadTo(_trim.End);
    }

    private void ToggleZoom()
    {
        if (_media == null)
        {
            return;
        }

        _trim.ToggleZoom();
        StartThumbs(_trim.WindowStart, _trim.WindowEnd - _trim.WindowStart, resetCache: false);
        UpdateStatus();
    }

    private void ToggleCropEdit()
    {
        if (_media == null || !_surface.HasFrame)
        {
            return;
        }

        if (_surface.IsEditingCrop)
        {
            _surface.EndCropEdit(keep: true);
        }
        else
        {
            Pause();
            _surface.BeginCropEdit();
        }

        UpdateChrome();
    }

    // =====================================================================================
    // Thumbnails
    // =====================================================================================

    private void StartThumbs(double start, double length, bool resetCache)
    {
        if (_media is not { } media || length <= 0)
        {
            return;
        }

        _thumbCancel?.Cancel();
        _thumbRevealTimer.Stop();
        _thumbRevision++;

        if (resetCache)
        {
            for (int i = 0; i < ThumbCount; i++)
            {
                _fullThumbs[i]?.Dispose();
                _fullThumbs[i] = null;
            }

            _fullThumbsComplete = false;
        }

        bool fullRange = start <= 0 && length >= media.Duration;
        _thumbStart = start;
        _thumbLength = length;

        // Release the visible zoomed strip (the full strip stays cached).
        for (int i = 0; i < ThumbCount; i++)
        {
            if (_thumbs[i] != null && !ReferenceEquals(_thumbs[i], _fullThumbs[i]))
            {
                _thumbs[i]!.Dispose();
            }

            _thumbs[i] = null;
        }

        // Zooming back out: restore the cached full-length strip instantly.
        if (fullRange && _fullThumbsComplete)
        {
            Array.Copy(_fullThumbs, _thumbs, ThumbCount);
            _thumbsAvailable = _thumbsReady = ThumbCount;
            _thumbsDone = true;
            _trimBar.SetThumbnails(_thumbs, _thumbsReady);
            return;
        }

        _thumbsAvailable = 0;
        _thumbsReady = 0;
        _thumbsDone = false;
        _trimBar.SetThumbnails(_thumbs, 0);

        int revision = _thumbRevision;
        var cancel = new CancellationTokenSource();
        _thumbCancel = cancel;
        string path = media.Path;

        _ = Task.Run(async () =>
        {
            try
            {
                await ThumbnailStrip.GenerateAsync(_tools, path, start, length, ThumbCount, ThumbHeight, (index, jpeg) =>
                {
                    Bitmap? bitmap = null;
                    if (jpeg != null)
                    {
                        try
                        {
                            using var stream = new MemoryStream(jpeg);
                            bitmap = new Bitmap(stream);
                        }
                        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or NotSupportedException)
                        {
                            bitmap = null;
                        }
                    }

                    Dispatcher.UIThread.Post(() => OnThumbReady(revision, index, bitmap, fullRange));
                }, cancel.Token);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (Exception ex) when (ex is IOException or InvalidOperationException or System.ComponentModel.Win32Exception)
            {
                Log("Thumbnails failed: " + ex.Message);
            }

            Dispatcher.UIThread.Post(() =>
            {
                if (revision == _thumbRevision)
                {
                    _thumbsDone = true;
                    if (_thumbsReady < _thumbsAvailable)
                    {
                        _thumbRevealTimer.Start();
                    }
                }
            });
        });
    }

    private void OnThumbReady(int revision, int index, Bitmap? bitmap, bool fullRange)
    {
        if (revision != _thumbRevision || _disposed)
        {
            bitmap?.Dispose();
            return;
        }

        _thumbs[index] = bitmap;
        if (fullRange)
        {
            _fullThumbs[index] = bitmap;
            if (index == ThumbCount - 1)
            {
                _fullThumbsComplete = true;
            }
        }

        _thumbsAvailable = Math.Max(_thumbsAvailable, index + 1);
        if (_thumbsReady == 0)
        {
            RevealNextThumb();
        }

        if (!_thumbRevealTimer.IsEnabled)
        {
            _thumbRevealTimer.Start();
        }
    }

    // Thumbs fade in one after another instead of popping in as a block, like omacut.
    private void RevealNextThumb()
    {
        if (_thumbsReady < _thumbsAvailable)
        {
            _thumbsReady++;
            _trimBar.SetThumbnails(_thumbs, _thumbsReady);
        }

        if (_thumbsReady >= _thumbsAvailable)
        {
            _thumbRevealTimer.Stop();
            if (_thumbsDone && _busyStatus == "Loading…")
            {
                _busyStatus = string.Empty;
                UpdateStatus();
            }
        }
    }

    // =====================================================================================
    // Export
    // =====================================================================================

    private (int Width, int Height) OutputFrame()
    {
        if (_media == null)
        {
            return (0, 0);
        }

        return _surface.Crop is CropRect crop ? (crop.Width, crop.Height) : (_media.Width, _media.Height);
    }

    private async void ShowExport()
    {
        if (_media == null || _exporting)
        {
            return;
        }

        Pause();
        if (_surface.IsEditingCrop)
        {
            _surface.EndCropEdit(keep: true);
        }

        EncoderSupport encoders = await _encoders;
        var formats = Enum.GetValues<ExportFormat>().Where(encoders.Supports).Select(f => (f, ExportFormats.DisplayName(f))).ToList();
        _formatChoice.SetItems(formats);
        _formatChoice.Selected = formats.Any(f => f.f == _lastFormat) ? _lastFormat : formats.FirstOrDefault().f;
        _audioChoice.SetItems([(true, "Keep"), (false, "Remove")]);

        _exportPath ??= UniquePath(ExportFormats.SuggestedPath(OutputBasePath(), _formatChoice.Selected));
        _exportError.IsVisible = false;
        _exportOptionsPanel.IsVisible = true;
        _exportProgressPanel.IsVisible = false;
        RefreshExportChoices();
        _exportOverlay.IsVisible = true;
        _exportGo.IsEnabled = formats.Count > 0;
        if (formats.Count == 0)
        {
            ShowExportError("This ffmpeg build cannot encode MP4, WebM or GIF.");
        }

        _exportGo.Focus();
        UpdateChrome();
    }

    private string OutputBasePath()
    {
        string source = _media!.Path;
        return string.IsNullOrWhiteSpace(_options.OutputDirectory)
            ? source
            : Path.Combine(_options.OutputDirectory, Path.GetFileName(source));
    }

    internal static string UniquePath(string path)
    {
        if (!File.Exists(path))
        {
            return path;
        }

        string directory = Path.GetDirectoryName(path) ?? string.Empty;
        string name = Path.GetFileNameWithoutExtension(path);
        string extension = Path.GetExtension(path);
        for (int i = 2; i < 1000; i++)
        {
            string candidate = Path.Combine(directory, $"{name} ({i}){extension}");
            if (!File.Exists(candidate))
            {
                return candidate;
            }
        }

        return path;
    }

    private void RefreshExportChoices()
    {
        if (_media == null)
        {
            return;
        }

        ExportFormat format = _formatChoice.Selected;
        (int width, int height) = OutputFrame();
        int previous = _sizeChoice.Selected;
        var sizes = new List<(int, string)> { (0, format == ExportFormat.Gif ? $"{width}×{height}" : $"Original") };
        sizes.AddRange(ExportFormats.ShortSides(format, width, height).Select(size => (size, size + "p")));
        _sizeChoice.SetItems(sizes);

        // GIFs at full size are enormous; default them to 480p (or the largest offered size).
        _sizeChoice.Selected = sizes.Any(s => s.Item1 == previous) && !(format == ExportFormat.Gif && previous == 0)
            ? previous
            : format == ExportFormat.Gif && sizes.Count > 1 ? sizes.Select(s => s.Item1).Where(s => s > 0 && s <= 480).DefaultIfEmpty(sizes[^1].Item1).First() : 0;

        _audioRow.IsVisible = format != ExportFormat.Gif && _media.HasAudio;

        if (_exportPath != null)
        {
            string withExtension = ExportFormats.WithExtension(_exportPath, format);
            if (withExtension != _exportPath)
            {
                _exportPath = File.Exists(withExtension) ? UniquePath(withExtension) : withExtension;
            }
        }

        RefreshExportSummary();
    }

    private void RefreshExportSummary()
    {
        if (_media == null)
        {
            return;
        }

        _exportPathText.Text = _exportPath;
        ToolTip.SetTip(_exportPathText, _exportPath);
        var parts = new List<string> { $"{TimeFormat.Format(_trim.Length)} clip" };
        if (_surface.Crop is CropRect crop)
        {
            parts.Add($"crop {crop.Width}×{crop.Height}");
        }

        if (_formatChoice.Selected != ExportFormat.Gif && _options.Watermark?.IsUsable == true)
        {
            parts.Add("watermark");
        }

        if (_formatChoice.Selected != ExportFormat.Gif && _media.HasAudio && !_audioChoice.Selected)
        {
            parts.Add("no audio");
        }

        _exportSummary.Text = string.Join(" · ", parts);
    }

    private async Task PickExportPathAsync()
    {
        if (_topLevel?.StorageProvider is not { } storage || _media == null)
        {
            return;
        }

        ExportFormat format = _formatChoice.Selected;
        string current = _exportPath ?? ExportFormats.SuggestedPath(OutputBasePath(), format);
        IStorageFolder? folder = null;
        try
        {
            folder = await storage.TryGetFolderFromPathAsync(Path.GetDirectoryName(current) ?? string.Empty);
        }
        catch (ArgumentException)
        {
        }

        string extension = ExportFormats.Extension(format).TrimStart('.');
        IStorageFile? file = await storage.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = "Export video",
            SuggestedFileName = Path.GetFileName(current),
            SuggestedStartLocation = folder,
            DefaultExtension = extension,
            ShowOverwritePrompt = true,
            FileTypeChoices = [new FilePickerFileType(ExportFormats.DisplayName(format)) { Patterns = ["*." + extension] }],
        });

        if (file?.TryGetLocalPath() is { } path)
        {
            _exportPath = path;
            RefreshExportSummary();
        }

        _exportGo.Focus();
    }

    private async Task RunExportAsync()
    {
        if (_media is not { } media || _exporting || _exportPath == null)
        {
            return;
        }

        var options = new ExportOptions
        {
            SourcePath = media.Path,
            Start = _trim.Start,
            End = _trim.End,
            Format = _formatChoice.Selected,
            ShortSide = _sizeChoice.Selected,
            IncludeAudio = _audioChoice.Selected,
            Crop = _surface.Crop,
            Watermark = _options.Watermark,
        };
        ExportState state = CurrentState;
        _lastFormat = options.Format;

        _exporting = true;
        _exportCancel = new CancellationTokenSource();
        _exportOptionsPanel.IsVisible = false;
        _exportProgressPanel.IsVisible = true;
        _progress.Value = 0;
        _progressText.Text = "Exporting 0%";
        _progressCancel.Focus();
        UpdateChrome();

        var progress = new Progress<double>(fraction =>
        {
            _progress.Value = fraction;
            _progressText.Text = $"Exporting {(int)Math.Round(fraction * 100)}%";
        });

        try
        {
            var service = new ExportService(_tools, await _encoders);
            string written = await service.ExportAsync(options, media, _exportPath, progress, _exportCancel.Token);
            _exported = state;
            LastExportedPath = written;
            _exportPath = null;
            _exporting = false;
            HideExport();
            ShowNotice("Saved " + written);
            Exported?.Invoke(this, written);

            if (_options.CloseAfterExport || _quitAfterExport)
            {
                ForceClose();
            }
        }
        catch (OperationCanceledException)
        {
            _exporting = false;
            _quitAfterExport = false;
            HideExport();
            ShowNotice("Export cancelled");
        }
        catch (ExportException ex)
        {
            _exporting = false;
            _quitAfterExport = false;
            Log("Export failed: " + ex.Message);
            _exportOptionsPanel.IsVisible = true;
            _exportProgressPanel.IsVisible = false;
            ShowExportError("Export failed: " + ex.Message);
            _exportGo.Focus();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            _exporting = false;
            _quitAfterExport = false;
            Log("Export failed: " + ex);
            _exportOptionsPanel.IsVisible = true;
            _exportProgressPanel.IsVisible = false;
            ShowExportError("Export failed: " + ex.Message);
        }
        finally
        {
            _exportCancel?.Dispose();
            _exportCancel = null;
            UpdateChrome();
        }
    }

    private void ShowExportError(string message)
    {
        _exportError.Text = message;
        _exportError.IsVisible = true;
    }

    private void HideExport()
    {
        if (_exporting)
        {
            return;
        }

        _exportOverlay.IsVisible = false;
        _quitAfterExport = false;
        Focus();
        UpdateChrome();
    }

    // =====================================================================================
    // Closing
    // =====================================================================================

    /// <summary>
    /// Asks to close. Returns true when the host may close now; otherwise the quit confirmation
    /// is showing (or an export is running) and the editor raises <see cref="CloseRequested"/>
    /// later if the user confirms.
    /// </summary>
    public bool RequestClose()
    {
        if (_exporting)
        {
            ShowNotice("Wait for the export to finish or cancel it first.");
            return false;
        }

        if (HasUnexportedEdit)
        {
            Pause();
            SetQuitVisible(true);
            return false;
        }

        return true;
    }

    private void ForceClose()
    {
        _quitVisible = false;
        _quitOverlay.IsVisible = false;
        Pause();
        _exported = CurrentState; // nothing left to warn about
        CloseRequested?.Invoke(this, EventArgs.Empty);
    }

    private void SetQuitVisible(bool visible)
    {
        _quitVisible = visible;
        _quitOverlay.IsVisible = visible;
        if (visible)
        {
            _quitExport.Focus();
        }
        else
        {
            Focus();
        }
    }

    private void SetHelpVisible(bool visible)
    {
        _helpVisible = visible;
        _helpOverlay.IsVisible = visible;
    }

    // =====================================================================================
    // Keyboard
    // =====================================================================================

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        _topLevel = TopLevel.GetTopLevel(this);
        _topLevel?.AddHandler(KeyDownEvent, OnKeyDown, RoutingStrategies.Tunnel);
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnDetachedFromVisualTree(e);
        _topLevel?.RemoveHandler(KeyDownEvent, OnKeyDown);
        _topLevel = null;
    }

    private void OnKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Handled)
        {
            return;
        }

        bool ctrl = e.KeyModifiers.HasFlag(KeyModifiers.Control) || e.KeyModifiers.HasFlag(KeyModifiers.Meta);
        bool shift = e.KeyModifiers.HasFlag(KeyModifiers.Shift);
        bool alt = e.KeyModifiers.HasFlag(KeyModifiers.Alt);
        string symbol = e.KeySymbol ?? string.Empty;

        // Overlays own the keyboard while they are up (buttons handle Enter/Space/Tab).
        if (_exportOverlay.IsVisible)
        {
            if (e.Key == Key.Escape)
            {
                if (_exporting)
                {
                    _exportCancel?.Cancel();
                }
                else
                {
                    HideExport();
                }

                e.Handled = true;
            }
            else if (e.Key == Key.Enter && !_exporting && FocusedPill() == null)
            {
                _ = RunExportAsync();
                e.Handled = true;
            }

            return;
        }

        if (_quitVisible)
        {
            if (e.Key == Key.Escape)
            {
                SetQuitVisible(false);
                e.Handled = true;
            }
            else if (e.Key is Key.Left or Key.Right)
            {
                MoveQuitFocus(e.Key == Key.Right ? 1 : -1);
                e.Handled = true;
            }

            return;
        }

        if (_helpVisible && (e.Key == Key.Escape || symbol == "?"))
        {
            SetHelpVisible(false);
            e.Handled = true;
            return;
        }

        if (symbol == "?")
        {
            SetHelpVisible(true);
            e.Handled = true;
            return;
        }

        if (_surface.IsEditingCrop)
        {
            switch (e.Key)
            {
                case Key.Escape:
                    _surface.EndCropEdit(keep: false);
                    UpdateChrome();
                    e.Handled = true;
                    return;
                case Key.Enter or Key.C:
                    _surface.EndCropEdit(keep: true);
                    UpdateChrome();
                    e.Handled = true;
                    return;
                case Key.Back or Key.Delete:
                    _surface.ResetCrop();
                    UpdateChrome();
                    e.Handled = true;
                    return;
            }
        }

        bool handled = true;
        switch (e.Key)
        {
            case Key.Space when ctrl:
                MoveTrimStartTo(_trim.Playhead);
                break;
            case Key.Space when alt:
                MoveTrimEndTo(_trim.Playhead);
                break;
            case Key.Space:
                TogglePlay();
                break;
            case Key.I when !ctrl:
                MoveTrimStartTo(_trim.Playhead);
                break;
            case Key.O when ctrl:
                if (_options.AllowOpeningOtherFiles)
                {
                    _ = OpenWithPickerAsync();
                }

                break;
            case Key.O:
                MoveTrimEndTo(_trim.Playhead);
                break;
            case Key.Left:
                SeekBy(shift ? -5 : alt ? -0.2 : -1);
                break;
            case Key.Right:
                SeekBy(shift ? 5 : alt ? 0.2 : 1);
                break;
            case Key.Home:
                MovePlayheadTo(_trim.Start);
                break;
            case Key.End:
                MovePlayheadTo(_trim.End);
                break;
            case Key.Z when !ctrl:
                ToggleZoom();
                break;
            case Key.C when !ctrl:
                ToggleCropEdit();
                break;
            case Key.M when !ctrl:
                ToggleMute();
                break;
            case Key.S when ctrl:
            case Key.E when ctrl:
                ShowExport();
                break;
            case Key.Q when !ctrl:
                if (RequestClose())
                {
                    CloseRequested?.Invoke(this, EventArgs.Empty);
                }

                break;
            case Key.Escape when _helpVisible:
                SetHelpVisible(false);
                break;
            default:
                handled = false;
                break;
        }

        if (!handled)
        {
            switch (symbol)
            {
                case ",":
                    StepFrame(-1);
                    handled = true;
                    break;
                case ".":
                    StepFrame(1);
                    handled = true;
                    break;
            }
        }

        e.Handled = handled;
    }

    private PillButton? FocusedPill() => _topLevel?.FocusManager?.GetFocusedElement() as PillButton;

    private void MoveQuitFocus(int direction)
    {
        PillButton[] order = [_quitCancel, _quitQuit, _quitExport];
        int index = Array.IndexOf(order, FocusedPill());
        order[((index < 0 ? 2 : index) + direction + order.Length) % order.Length].Focus();
    }

    // =====================================================================================
    // Chrome, status and accent
    // =====================================================================================

    private void UpdateChrome()
    {
        bool hasVideo = _media != null;
        _layout.Margin = new Thickness(hasVideo ? 16 : 0);
        _controlsRow.IsVisible = hasVideo;
        _status.IsVisible = hasVideo;
        _openButton.IsVisible = !hasVideo;
        _openHint.IsVisible = !hasVideo;
        _openButton.IsEnabled = _options.AllowOpeningOtherFiles;

        _playButton.Kind = IsPlaying ? IconKind.Pause : IconKind.Play;
        _playButton.Tip = IsPlaying ? "Pause (Space)" : "Play (Space)";
        _playButton.IsEnabled = hasVideo && !_surface.IsEditingCrop;
        _muteButton.Kind = _engine.Muted || !_engine.HasAudioOutput ? IconKind.VolumeOff : IconKind.VolumeOn;
        _muteButton.IsVisible = hasVideo && _media!.HasAudio;
        _muteButton.IsEnabled = _engine.HasAudioOutput;
        _muteButton.Tip = !_engine.HasAudioOutput ? "Audio output unavailable" : _engine.Muted ? "Unmute (M)" : "Mute (M)";
        _cropButton.Active = _surface.IsEditingCrop;
        _cropButton.Tip = _surface.IsEditingCrop ? "Apply crop (Enter)" : "Crop (C)";
        _exportButton.IsEnabled = hasVideo && !_exporting;
        UpdateStatus();
    }

    private void UpdateStatus()
    {
        if (_media == null)
        {
            _status.Text = string.Empty;
            return;
        }

        _status.Inlines?.Clear();
        if (_notice.Length > 0)
        {
            _status.Text = _notice;
            _status.Foreground = new ImmutableSolidColorBrush(_accent);
            return;
        }

        if (_busyStatus.Length > 0)
        {
            _status.Text = _busyStatus;
            _status.Foreground = Palette.TextMutedBrush;
            return;
        }

        if (_surface.IsEditingCrop)
        {
            _status.Text = "Drag to crop · Enter apply · Esc cancel · Backspace reset";
            _status.Foreground = Palette.TextMutedBrush;
            return;
        }

        if (_trimBar.IsTrimmingRange)
        {
            _status.Text = string.Empty;
            return;
        }

        _status.Text = null;
        _status.Foreground = Palette.TextSecondaryBrush;
        var inlines = new InlineCollection
        {
            new Run(TimeFormat.Format(_trim.Playhead) + " (" + TimeFormat.Format(_trim.Length) + ")"),
        };

        var accentBrush = new ImmutableSolidColorBrush(_accent);
        if (_trim.Zoomed)
        {
            inlines.Add(new Run(" · "));
            inlines.Add(new Run("zoomed") { Foreground = accentBrush });
        }

        if (_surface.Crop is CropRect crop)
        {
            inlines.Add(new Run(" · "));
            inlines.Add(new Run($"crop {crop.Width}×{crop.Height}") { Foreground = accentBrush });
        }

        if (_engine.Muted)
        {
            inlines.Add(new Run(" · muted"));
        }

        _status.Inlines = inlines;
    }

    private void ShowNotice(string text)
    {
        _notice = text;
        _noticeTimer.Stop();
        _noticeTimer.Start();
        UpdateStatus();
    }

    private void ApplyAccent(string color)
    {
        _accent = Color.Parse(color);
        _accentForeground = Color.Parse(OmarchyTheme.ForegroundFor(color));
        _trimBar.Accent = _accent;
        _surface.Accent = _accent;
        _progress.Accent = _accent;
        foreach (IconButton button in new[] { _playButton, _muteButton, _cropButton, _exportButton })
        {
            button.Accent = _accent;
            button.AccentForeground = _accentForeground;
        }

        foreach (PillButton pill in new[] { _openButton, _quitCancel, _quitQuit, _quitExport, _exportGo, _exportCancelButton, _exportChange, _progressCancel })
        {
            pill.Accent = _accent;
            pill.AccentForeground = _accentForeground;
            pill.Refresh();
        }

        _formatChoice.Accent = _sizeChoice.Accent = _audioChoice.Accent = _accent;
        _formatChoice.AccentForeground = _sizeChoice.AccentForeground = _audioChoice.AccentForeground = _accentForeground;
        _formatChoice.Refresh();
        _sizeChoice.Refresh();
        _audioChoice.Refresh();
        BuildHelp();
        UpdateStatus();
    }

    private void BuildHelp()
    {
        (string Keys, string Action)[] rows =
        [
            ("Space", "Play / pause"),
            ("← / →", "Move playhead 1s"),
            ("Shift ← / →", "Move playhead 5s"),
            ("Alt ← / →", "Move playhead 0.2s"),
            (", / .", "Previous / next frame"),
            ("Ctrl Space  or  I", "Trim start to playhead"),
            ("Alt Space  or  O", "Trim end to playhead"),
            ("Home / End", "Playhead to trim start / end"),
            ("Z", "Zoom the selection"),
            ("C", "Crop (Enter apply, Esc cancel)"),
            ("M", "Mute"),
            ("Ctrl O", "Open a video"),
            ("Ctrl S", "Export"),
            ("Q", "Quit"),
            ("?", "Show these shortcuts"),
        ];

        _helpList.Children.Clear();
        _helpList.Children.Add(new TextBlock { Text = "Keyboard shortcuts", FontSize = 16, FontWeight = FontWeight.SemiBold, Foreground = Palette.TextBrush, Margin = new Thickness(0, 0, 0, 8) });
        var accent = new ImmutableSolidColorBrush(_accent);
        foreach ((string keys, string action) in rows)
        {
            _helpList.Children.Add(new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 18,
                Children =
                {
                    new TextBlock { Text = keys, Width = 150, TextAlignment = TextAlignment.Right, Foreground = accent, FontSize = 13, FontFamily = Palette.Mono },
                    new TextBlock { Text = action, Foreground = Palette.TextSecondaryBrush, FontSize = 13 },
                },
            });
        }
    }

    // =====================================================================================
    // Layout helpers
    // =====================================================================================

    private static void AddToGrid(Grid grid, Control control, int index, bool row = false)
    {
        if (row)
        {
            Grid.SetRow(control, index);
        }
        else
        {
            Grid.SetColumn(control, index);
        }

        grid.Children.Add(control);
    }

    private static Control Dock(Control control, Avalonia.Controls.Dock dock)
    {
        DockPanel.SetDock(control, dock);
        control.Margin = new Thickness(dock == Avalonia.Controls.Dock.Right ? 10 : 0, 0, 0, 0);
        return control;
    }

    private static TextBlock Heading(string text) =>
        new() { Text = text, FontSize = 16, FontWeight = FontWeight.SemiBold, Foreground = Palette.TextBrush };

    private static StackPanel OptionRow(string label, Control content) => new()
    {
        Orientation = Orientation.Horizontal,
        Spacing = 14,
        Children =
        {
            new TextBlock { Text = label, Width = 64, Foreground = Palette.TextMutedBrush, FontSize = 13, VerticalAlignment = VerticalAlignment.Center },
            content,
        },
    };

    private static Border Card(Control content) => new()
    {
        Background = Palette.FilmBrush,
        CornerRadius = new CornerRadius(12),
        Padding = new Thickness(28, 24),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        Child = content,
    };

    private static Border Overlay(Border card, Action onScrimClick)
    {
        card.PointerPressed += (_, e) => e.Handled = true; // clicks inside the card never dismiss it
        var overlay = new Border { Background = Palette.ScrimBrush, IsVisible = false, Child = card };
        overlay.PointerPressed += (_, e) =>
        {
            e.Handled = true;
            onScrimClick();
        };
        return overlay;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _exportCancel?.Cancel();
        _thumbCancel?.Cancel();
        _renderTimer.Stop();
        _thumbRevealTimer.Stop();
        _noticeTimer.Stop();
        _engine.Dispose();
        _theme?.Dispose();
        _surface.Clear();
        for (int i = 0; i < ThumbCount; i++)
        {
            if (_thumbs[i] != null && !ReferenceEquals(_thumbs[i], _fullThumbs[i]))
            {
                _thumbs[i]!.Dispose();
            }

            _fullThumbs[i]?.Dispose();
            _thumbs[i] = _fullThumbs[i] = null;
        }
    }
}
