using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Immutable;

namespace Omacut.Controls;

internal enum IconKind
{
    Play,
    Pause,
    Export,
    Crop,
    VolumeOn,
    VolumeOff,
}

/// <summary>omacut's round 44px icon button, drawn with vector glyphs.</summary>
internal sealed class IconButton : Control
{
    private static readonly Geometry PlayGlyph = Geometry.Parse("M8,5 L8,19 L19,12 Z");
    private static readonly Geometry PauseGlyph = Geometry.Parse("M7,5 h4 v14 h-4 Z M14,5 h4 v14 h-4 Z");
    private static readonly Geometry ExportGlyph = Geometry.Parse("M12,4 L12,14 M7,10 L12,15 L17,10 M6,20 L18,20");
    private static readonly Geometry CropGlyph = Geometry.Parse("M6,2 L6,18 L22,18 M2,6 L18,6 L18,22");
    private static readonly Geometry SpeakerGlyph = Geometry.Parse("M4,9 L8,9 L13,5 L13,19 L8,15 L4,15 Z");
    private static readonly Geometry WavesGlyph = Geometry.Parse("M16.5,9 C17.8,10.4 17.8,13.6 16.5,15 M19,6.5 C21.6,9.3 21.6,14.7 19,17.5");
    private static readonly Geometry MuteGlyph = Geometry.Parse("M16.5,9.5 L21,14 M21,9.5 L16.5,14");

    private IconKind _kind;
    private bool _hover;
    private bool _active;

    public IconButton(IconKind kind, string tip)
    {
        _kind = kind;
        Width = 44;
        Height = 44;
        Cursor = new Cursor(StandardCursorType.Hand);
        ToolTip.SetTip(this, tip);
        Focusable = false;
    }

    public event EventHandler? Click;

    public IconKind Kind
    {
        get => _kind;
        set
        {
            _kind = value;
            InvalidateVisual();
        }
    }

    /// <summary>Highlights the button with the accent (e.g. crop editing).</summary>
    public bool Active
    {
        get => _active;
        set
        {
            _active = value;
            InvalidateVisual();
        }
    }

    public Color Accent { get; set; } = Colors.Gold;

    public Color AccentForeground { get; set; } = Colors.Black;

    public string Tip
    {
        set => ToolTip.SetTip(this, value);
    }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == IsEnabledProperty)
        {
            InvalidateVisual();
        }
    }

    public override void Render(DrawingContext context)
    {
        double opacity = IsEffectivelyEnabled ? 1 : 0.45;
        using (context.PushOpacity(opacity))
        {
            IBrush background = _active ? new ImmutableSolidColorBrush(Accent) : _hover && IsEffectivelyEnabled ? Palette.SurfaceHoverBrush : Palette.SurfaceBrush;
            context.DrawEllipse(background, null, new Rect(Bounds.Size));

            var ink = new ImmutableSolidColorBrush(_active ? AccentForeground : Colors.White);
            var pen = new ImmutablePen(ink, 2.4, lineCap: PenLineCap.Round, lineJoin: PenLineJoin.Round);
            using (context.PushTransform(Matrix.CreateTranslation((Bounds.Width - 24) / 2, (Bounds.Height - 24) / 2)))
            {
                switch (_kind)
                {
                    case IconKind.Play:
                        context.DrawGeometry(ink, null, PlayGlyph);
                        break;
                    case IconKind.Pause:
                        context.DrawGeometry(ink, null, PauseGlyph);
                        break;
                    case IconKind.Export:
                        context.DrawGeometry(null, pen, ExportGlyph);
                        break;
                    case IconKind.Crop:
                        context.DrawGeometry(null, pen, CropGlyph);
                        break;
                    case IconKind.VolumeOn:
                        context.DrawGeometry(ink, pen, SpeakerGlyph);
                        context.DrawGeometry(null, pen, WavesGlyph);
                        break;
                    case IconKind.VolumeOff:
                        context.DrawGeometry(ink, pen, SpeakerGlyph);
                        context.DrawGeometry(null, pen, MuteGlyph);
                        break;
                }
            }
        }
    }

    protected override void OnPointerEntered(PointerEventArgs e)
    {
        base.OnPointerEntered(e);
        _hover = true;
        InvalidateVisual();
    }

    protected override void OnPointerExited(PointerEventArgs e)
    {
        base.OnPointerExited(e);
        _hover = false;
        InvalidateVisual();
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (IsEffectivelyEnabled && e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            e.Handled = true;
            Click?.Invoke(this, EventArgs.Empty);
        }
    }
}

/// <summary>A focusable rounded text button for dialogs; Enter/Space activate it.</summary>
internal sealed class PillButton : Border
{
    private readonly TextBlock _label;
    private bool _primary;
    private bool _hover;

    public PillButton(string text, bool primary = false)
    {
        _primary = primary;
        _label = new TextBlock
        {
            Text = text,
            FontSize = 13,
            FontWeight = FontWeight.SemiBold,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        Child = _label;
        Height = 34;
        MinWidth = 72;
        Padding = new Thickness(14, 0);
        CornerRadius = new CornerRadius(8);
        Focusable = true;
        Cursor = new Cursor(StandardCursorType.Hand);
        Refresh();
    }

    public event EventHandler? Click;

    public Color Accent { get; set; } = Colors.Gold;
    public Color AccentForeground { get; set; } = Colors.Black;

    public string Text
    {
        get => _label.Text ?? string.Empty;
        set => _label.Text = value;
    }

    public bool Primary
    {
        get => _primary;
        set
        {
            _primary = value;
            Refresh();
        }
    }

    public void Refresh()
    {
        bool enabled = IsEffectivelyEnabled;
        Background = _primary
            ? new ImmutableSolidColorBrush(Accent)
            : _hover && enabled ? Palette.SurfaceHoverBrush : Palette.SurfaceBrush;
        _label.Foreground = _primary ? new ImmutableSolidColorBrush(AccentForeground) : Palette.TextBrush;
        BorderBrush = IsFocused ? new ImmutableSolidColorBrush(_primary ? AccentForeground : Accent) : Brushes.Transparent;
        BorderThickness = new Thickness(IsFocused ? 2 : 0);
        Opacity = enabled ? 1 : 0.45;
    }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == IsFocusedProperty || change.Property == IsEnabledProperty)
        {
            Refresh();
        }
    }

    protected override void OnPointerEntered(PointerEventArgs e)
    {
        base.OnPointerEntered(e);
        _hover = true;
        Refresh();
    }

    protected override void OnPointerExited(PointerEventArgs e)
    {
        base.OnPointerExited(e);
        _hover = false;
        Refresh();
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (IsEffectivelyEnabled && e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            e.Handled = true;
            Click?.Invoke(this, EventArgs.Empty);
        }
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (IsEffectivelyEnabled && e.Key is Key.Enter or Key.Space)
        {
            e.Handled = true;
            Click?.Invoke(this, EventArgs.Empty);
        }
    }
}

/// <summary>A row of mutually exclusive choices (export format, size).</summary>
internal sealed class Segmented<T> : StackPanel
{
    private readonly List<(T Value, PillButton Button)> _items = [];
    private T _selected;

    public Segmented(T initial)
    {
        _selected = initial;
        Orientation = Orientation.Horizontal;
        Spacing = 6;
    }

    public event EventHandler? SelectionChanged;

    public Color Accent { get; set; } = Colors.Gold;
    public Color AccentForeground { get; set; } = Colors.Black;

    public T Selected
    {
        get => _selected;
        set
        {
            _selected = value;
            Refresh();
        }
    }

    public void SetItems(IEnumerable<(T Value, string Label)> items)
    {
        Children.Clear();
        _items.Clear();
        foreach ((T value, string label) in items)
        {
            var button = new PillButton(label) { MinWidth = 56, Height = 30 };
            button.Click += (_, _) =>
            {
                _selected = value;
                Refresh();
                SelectionChanged?.Invoke(this, EventArgs.Empty);
            };
            _items.Add((value, button));
            Children.Add(button);
        }

        if (_items.Count > 0 && !_items.Any(item => EqualityComparer<T>.Default.Equals(item.Value, _selected)))
        {
            _selected = _items[0].Value;
        }

        Refresh();
    }

    public void Refresh()
    {
        foreach ((T value, PillButton button) in _items)
        {
            button.Accent = Accent;
            button.AccentForeground = AccentForeground;
            button.Primary = EqualityComparer<T>.Default.Equals(value, _selected);
        }
    }
}
