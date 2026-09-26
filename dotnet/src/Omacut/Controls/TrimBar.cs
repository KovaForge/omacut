using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using Avalonia.Media.Imaging;
using Omacut.Core;

namespace Omacut.Controls;

/// <summary>
/// A thumbnail filmstrip with two draggable handles and a scrubbable playhead, drawn directly.
/// Hit-testing, the minimum gap and the playhead parking follow omacut's TrimBar.qml.
/// </summary>
internal sealed class TrimBar : Control
{
    private const double HandleWidth = 14;

    private enum Mode
    {
        None,
        Start,
        End,
        Playhead,
    }

    private readonly TrimModel _model;
    private Bitmap?[] _thumbs = [];
    private int _thumbsReady;
    private Mode _mode;
    private Color _accent = Color.Parse(OmarchyTheme.DefaultAccent);
    private IBrush _accentBrush;
    private IPen _framePen;
    private static readonly Cursor ResizeCursor = new(StandardCursorType.SizeWestEast);

    public TrimBar(TrimModel model)
    {
        _model = model;
        _model.Changed += (_, _) => InvalidateVisual();
        _accentBrush = new ImmutableSolidColorBrush(_accent);
        _framePen = new ImmutablePen(_accentBrush.ToImmutable(), 3);
        Height = 76;
        ClipToBounds = false;
        Focusable = false;
    }

    /// <summary>Raised while the user drags, with the time to preview.</summary>
    public event EventHandler<double>? Scrubbed;

    /// <summary>Raised when a drag starts (true) or ends (false).</summary>
    public event EventHandler<bool>? InteractionChanged;

    public bool IsInteracting => _mode != Mode.None;
    public bool IsTrimmingRange => _mode is Mode.Start or Mode.End;

    public Color Accent
    {
        get => _accent;
        set
        {
            _accent = value;
            _accentBrush = new ImmutableSolidColorBrush(value);
            _framePen = new ImmutablePen(_accentBrush.ToImmutable(), 3);
            InvalidateVisual();
        }
    }

    public void SetThumbnails(Bitmap?[] thumbs, int ready)
    {
        _thumbs = thumbs;
        _thumbsReady = ready;
        InvalidateVisual();
    }

    private double TrackX => HandleWidth;
    private double TrackWidth => Math.Max(Bounds.Width - 2 * HandleWidth, 1);
    private double X(double time) => _model.XForTime(time, TrackX, TrackWidth);
    private double Time(double x) => _model.TimeForX(x, TrackX, TrackWidth);

    public override void Render(DrawingContext context)
    {
        double height = Bounds.Height;
        var track = new Rect(TrackX, 4, TrackWidth, Math.Max(height - 8, 1));

        // ---- filmstrip ----
        using (context.PushClip(new RoundedRect(track, 6)))
        {
            context.FillRectangle(Palette.FilmBrush, track);
            int count = _thumbs.Length;
            if (count > 0)
            {
                double slot = track.Width / count;
                for (int i = 0; i < Math.Min(_thumbsReady, count); i++)
                {
                    if (_thumbs[i] is not { } thumb)
                    {
                        continue;
                    }

                    var dest = new Rect(track.X + i * slot, track.Y, slot, track.Height);
                    context.DrawImage(thumb, CoverSource(thumb.Size, dest.Size), dest);
                }
            }
        }

        if (_model.Duration <= 0)
        {
            return;
        }

        double x0 = X(_model.Start);
        double x1 = X(_model.End);

        // ---- dim outside the selection ----
        context.FillRectangle(Palette.DimBrush, new Rect(track.X, track.Y, Math.Max(0, x0 - track.X), track.Height));
        context.FillRectangle(Palette.DimBrush, new Rect(x1, track.Y, Math.Max(0, track.Right - x1), track.Height));

        // ---- selection frame ----
        var frame = new Rect(x0 - HandleWidth, 0, x1 - x0 + 2 * HandleWidth, height);
        context.DrawRectangle(null, _framePen, new RoundedRect(frame.Deflate(1.5), 8));

        // ---- handles ----
        DrawHandle(context, new Rect(x0 - HandleWidth, 0, HandleWidth, height));
        DrawHandle(context, new Rect(x1, 0, HandleWidth, height));

        // ---- playhead ----
        if (_model.Playhead >= _model.WindowStart && _model.Playhead <= _model.WindowEnd)
        {
            context.FillRectangle(Brushes.White, new Rect(X(_model.Playhead) - 1, track.Y, 2, track.Height));
        }

        // ---- floating time bubble while adjusting a handle ----
        if (IsTrimmingRange)
        {
            double time = _mode == Mode.Start ? _model.Start : _model.End;
            double handleX = _mode == Mode.Start ? x0 - HandleWidth / 2 : x1 + HandleWidth / 2;
            var bubble = new Rect(Math.Clamp(handleX - 41, 0, Math.Max(0, Bounds.Width - 82)), -42, 82, 32);
            context.DrawRectangle(Palette.SurfaceBrush, null, new RoundedRect(bubble, 7));
            var text = new FormattedText(
                TimeFormat.Format(time), CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                new Typeface(Palette.Mono, FontStyle.Normal, FontWeight.SemiBold), 15, Brushes.White);
            context.DrawText(text, new Point(bubble.X + (bubble.Width - text.Width) / 2, bubble.Y + (bubble.Height - text.Height) / 2));
            context.FillRectangle(Brushes.White, new Rect(handleX - 1, bubble.Bottom, 2, Math.Max(0, track.Y - bubble.Bottom)));
        }
    }

    private void DrawHandle(DrawingContext context, Rect rect)
    {
        context.DrawRectangle(_accentBrush, null, new RoundedRect(rect, 5));
        context.DrawRectangle(Palette.FilmBrush, null, new RoundedRect(new Rect(rect.Center.X - 1, rect.Center.Y - 8, 2, 16), 1));
    }

    // PreserveAspectCrop: the centered part of the thumbnail that fills the slot.
    private static Rect CoverSource(Size image, Size slot)
    {
        if (image.Width <= 0 || image.Height <= 0 || slot.Width <= 0 || slot.Height <= 0)
        {
            return new Rect(image);
        }

        double scale = Math.Max(slot.Width / image.Width, slot.Height / image.Height);
        double width = slot.Width / scale;
        double height = slot.Height / scale;
        return new Rect((image.Width - width) / 2, (image.Height - height) / 2, width, height);
    }

    private Mode HitTest(double x)
    {
        double x0 = X(_model.Start);
        double x1 = X(_model.End);
        if (Math.Abs(x - (x0 - HandleWidth / 2)) <= HandleWidth)
        {
            return Mode.Start;
        }

        if (Math.Abs(x - (x1 + HandleWidth / 2)) <= HandleWidth)
        {
            return Mode.End;
        }

        return x >= x0 && x <= x1 ? Mode.Playhead : Mode.None;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (_model.Duration <= 0 || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            return;
        }

        double x = e.GetPosition(this).X;
        _mode = HitTest(x);
        if (_mode == Mode.None)
        {
            return;
        }

        e.Pointer.Capture(this);
        e.Handled = true;
        InteractionChanged?.Invoke(this, true);
        if (_mode == Mode.Playhead)
        {
            _model.SetPlayhead(Time(x));
            Scrubbed?.Invoke(this, _model.Playhead);
        }

        InvalidateVisual();
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        if (_model.Duration <= 0)
        {
            return;
        }

        double x = e.GetPosition(this).X;
        if (_mode == Mode.None)
        {
            Mode hover = HitTest(x);
            Cursor = hover is Mode.Start or Mode.End ? ResizeCursor : Cursor.Default;
            return;
        }

        double time = Time(x);
        switch (_mode)
        {
            case Mode.Start:
                _model.MoveStartTo(time);
                break;
            case Mode.End:
                _model.MoveEndTo(time);
                break;
            default:
                _model.SetPlayhead(time);
                break;
        }

        Scrubbed?.Invoke(this, _model.Playhead);
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        EndInteraction(e.Pointer);
    }

    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e)
    {
        base.OnPointerCaptureLost(e);
        EndInteraction(null);
    }

    private void EndInteraction(IPointer? pointer)
    {
        if (_mode == Mode.None)
        {
            return;
        }

        _mode = Mode.None;
        pointer?.Capture(null);
        InteractionChanged?.Invoke(this, false);
        InvalidateVisual();
    }
}
