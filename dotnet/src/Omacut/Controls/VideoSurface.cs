using System.Globalization;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Omacut.Core;

namespace Omacut.Controls;

/// <summary>
/// Shows the preview frame letterboxed in a rounded black card and hosts crop editing.
/// Crop coordinates are in displayed source pixels; with a crop applied (and not being edited)
/// the preview shows only the cropped area, so what you see is what you export.
/// </summary>
internal sealed class VideoSurface : Control
{
    private const double CornerRadius = 12;
    private const double HandleHit = 12;
    private const int MinCropPixels = 16;

    [Flags]
    private enum Edge
    {
        None = 0,
        Left = 1,
        Top = 2,
        Right = 4,
        Bottom = 8,
        Move = 16,
        New = 32,
    }

    private WriteableBitmap? _bitmap;
    private int _sourceWidth;
    private int _sourceHeight;
    private CropRect? _crop;
    private CropRect? _cropBeforeEdit;
    private bool _editingCrop;
    private Edge _drag;
    private Point _dragStart;
    private CropRect _dragOrigin;
    private Color _accent = Color.Parse(OmarchyTheme.DefaultAccent);
    private IBrush _accentBrush = new ImmutableSolidColorBrush(Color.Parse(OmarchyTheme.DefaultAccent));
    private static readonly IPen ThirdsPen = new ImmutablePen(new ImmutableSolidColorBrush(Color.Parse("#55ffffff")), 1);

    public VideoSurface()
    {
        ClipToBounds = true;
        Focusable = false;
    }

    /// <summary>A click on the picture outside crop editing (play/pause).</summary>
    public event EventHandler? Clicked;

    /// <summary>The crop changed (including being cleared).</summary>
    public event EventHandler? CropChanged;

    public bool HasFrame => _bitmap != null;

    public Color Accent
    {
        get => _accent;
        set
        {
            _accent = value;
            _accentBrush = new ImmutableSolidColorBrush(value);
            InvalidateVisual();
        }
    }

    public CropRect? Crop
    {
        get => _crop;
        set
        {
            _crop = value is { IsEmpty: false } crop && !crop.CoversFrame(_sourceWidth, _sourceHeight) ? crop : null;
            InvalidateVisual();
            CropChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public bool IsEditingCrop => _editingCrop;

    public void SetSource(int sourceWidth, int sourceHeight)
    {
        _sourceWidth = sourceWidth;
        _sourceHeight = sourceHeight;
        _crop = null;
        _editingCrop = false;
        InvalidateVisual();
    }

    public void Clear()
    {
        _bitmap?.Dispose();
        _bitmap = null;
        _sourceWidth = _sourceHeight = 0;
        _crop = null;
        _editingCrop = false;
        InvalidateVisual();
    }

    /// <summary>Copies a BGRA frame into the display bitmap (UI thread).</summary>
    public void ShowFrame(byte[] pixels, int width, int height)
    {
        if (_bitmap == null || _bitmap.PixelSize.Width != width || _bitmap.PixelSize.Height != height)
        {
            _bitmap?.Dispose();
            _bitmap = new WriteableBitmap(new PixelSize(width, height), new Vector(96, 96), PixelFormat.Bgra8888, AlphaFormat.Premul);
        }

        using (ILockedFramebuffer buffer = _bitmap.Lock())
        {
            int rowBytes = width * 4;
            if (buffer.RowBytes == rowBytes)
            {
                Marshal.Copy(pixels, 0, buffer.Address, rowBytes * height);
            }
            else
            {
                for (int y = 0; y < height; y++)
                {
                    Marshal.Copy(pixels, y * rowBytes, buffer.Address + y * buffer.RowBytes, rowBytes);
                }
            }
        }

        InvalidateVisual();
    }

    public void BeginCropEdit()
    {
        if (_sourceWidth <= 0)
        {
            return;
        }

        _cropBeforeEdit = _crop;
        _editingCrop = true;
        _crop ??= new CropRect(0, 0, _sourceWidth, _sourceHeight);
        InvalidateVisual();
    }

    /// <summary>Leaves crop editing; <paramref name="keep"/> false restores the crop from before.</summary>
    public void EndCropEdit(bool keep)
    {
        if (!_editingCrop)
        {
            return;
        }

        _editingCrop = false;
        _drag = Edge.None;
        Crop = keep ? _crop : _cropBeforeEdit;
    }

    public void ResetCrop()
    {
        if (_editingCrop)
        {
            _crop = new CropRect(0, 0, _sourceWidth, _sourceHeight);
            InvalidateVisual();
        }
        else
        {
            Crop = null;
        }
    }

    // ---- geometry ----

    /// <summary>The source-pixel region currently shown.</summary>
    private CropRect ShownRegion =>
        !_editingCrop && _crop is CropRect crop ? crop : new CropRect(0, 0, _sourceWidth, _sourceHeight);

    private Rect DestinationRect()
    {
        CropRect shown = ShownRegion;
        if (shown.IsEmpty || Bounds.Width <= 0 || Bounds.Height <= 0)
        {
            return default;
        }

        double scale = Math.Min(Bounds.Width / shown.Width, Bounds.Height / shown.Height);
        double width = shown.Width * scale;
        double height = shown.Height * scale;
        return new Rect((Bounds.Width - width) / 2, (Bounds.Height - height) / 2, width, height);
    }

    private double ViewScale => DestinationRect().Width / Math.Max(ShownRegion.Width, 1);

    private Rect ToView(CropRect crop)
    {
        Rect dest = DestinationRect();
        double scale = ViewScale;
        return new Rect(dest.X + crop.X * scale, dest.Y + crop.Y * scale, crop.Width * scale, crop.Height * scale);
    }

    private Point ToSource(Point view)
    {
        Rect dest = DestinationRect();
        double scale = ViewScale;
        return scale <= 0 ? default : new Point((view.X - dest.X) / scale, (view.Y - dest.Y) / scale);
    }

    // ---- rendering ----

    public override void Render(DrawingContext context)
    {
        var bounds = new Rect(Bounds.Size);
        using (context.PushClip(new RoundedRect(bounds, CornerRadius)))
        {
            context.FillRectangle(Brushes.Black, bounds);
            if (_bitmap == null || _sourceWidth <= 0)
            {
                return;
            }

            Rect dest = DestinationRect();
            CropRect shown = ShownRegion;
            double toFrameX = _bitmap.PixelSize.Width / (double)_sourceWidth;
            double toFrameY = _bitmap.PixelSize.Height / (double)_sourceHeight;
            var source = new Rect(shown.X * toFrameX, shown.Y * toFrameY, shown.Width * toFrameX, shown.Height * toFrameY);
            context.DrawImage(_bitmap, source, dest);

            if (_editingCrop && _crop is CropRect crop)
            {
                RenderCropEditor(context, dest, ToView(crop), crop);
            }
        }
    }

    private void RenderCropEditor(DrawingContext context, Rect dest, Rect rect, CropRect crop)
    {
        // Dim everything outside the crop.
        context.FillRectangle(Palette.DimBrush, new Rect(dest.X, dest.Y, dest.Width, Math.Max(0, rect.Y - dest.Y)));
        context.FillRectangle(Palette.DimBrush, new Rect(dest.X, rect.Bottom, dest.Width, Math.Max(0, dest.Bottom - rect.Bottom)));
        context.FillRectangle(Palette.DimBrush, new Rect(dest.X, rect.Y, Math.Max(0, rect.X - dest.X), rect.Height));
        context.FillRectangle(Palette.DimBrush, new Rect(rect.Right, rect.Y, Math.Max(0, dest.Right - rect.Right), rect.Height));

        for (int i = 1; i <= 2; i++)
        {
            double x = rect.X + rect.Width * i / 3;
            double y = rect.Y + rect.Height * i / 3;
            context.DrawLine(ThirdsPen, new Point(x, rect.Y), new Point(x, rect.Bottom));
            context.DrawLine(ThirdsPen, new Point(rect.X, y), new Point(rect.Right, y));
        }

        context.DrawRectangle(null, new ImmutablePen(_accentBrush.ToImmutable(), 2), rect);

        foreach (Point corner in new[] { rect.TopLeft, rect.TopRight, rect.BottomLeft, rect.BottomRight })
        {
            context.DrawRectangle(_accentBrush, null, new RoundedRect(new Rect(corner.X - 6, corner.Y - 6, 12, 12), 3));
        }

        foreach (Point mid in new[] { new Point(rect.Center.X, rect.Y), new Point(rect.Center.X, rect.Bottom), new Point(rect.X, rect.Center.Y), new Point(rect.Right, rect.Center.Y) })
        {
            context.DrawRectangle(_accentBrush, null, new RoundedRect(new Rect(mid.X - 5, mid.Y - 5, 10, 10), 5));
        }

        var label = new FormattedText(
            string.Create(CultureInfo.InvariantCulture, $"{crop.Width} × {crop.Height}"),
            CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
            new Typeface(Palette.Mono), 12, Brushes.White);
        var pill = new Rect(rect.X + 8, rect.Y + 8, label.Width + 14, label.Height + 6);
        if (pill.Right < rect.Right && pill.Bottom < rect.Bottom)
        {
            context.DrawRectangle(Palette.ScrimBrush, null, new RoundedRect(pill, 5));
            context.DrawText(label, new Point(pill.X + 7, pill.Y + 3));
        }
    }

    // ---- interaction ----

    private Edge HitCrop(Point p)
    {
        if (_crop is not CropRect crop)
        {
            return Edge.New;
        }

        Rect r = ToView(crop);
        Edge edge = Edge.None;
        bool withinY = p.Y >= r.Y - HandleHit && p.Y <= r.Bottom + HandleHit;
        bool withinX = p.X >= r.X - HandleHit && p.X <= r.Right + HandleHit;
        if (withinY && Math.Abs(p.X - r.X) <= HandleHit)
        {
            edge |= Edge.Left;
        }
        else if (withinY && Math.Abs(p.X - r.Right) <= HandleHit)
        {
            edge |= Edge.Right;
        }

        if (withinX && Math.Abs(p.Y - r.Y) <= HandleHit)
        {
            edge |= Edge.Top;
        }
        else if (withinX && Math.Abs(p.Y - r.Bottom) <= HandleHit)
        {
            edge |= Edge.Bottom;
        }

        if (edge != Edge.None)
        {
            return edge;
        }

        return r.Contains(p) ? Edge.Move : Edge.New;
    }

    private static Cursor CursorFor(Edge edge) => edge switch
    {
        Edge.Left | Edge.Top or Edge.Right | Edge.Bottom => new Cursor(StandardCursorType.TopLeftCorner),
        Edge.Right | Edge.Top or Edge.Left | Edge.Bottom => new Cursor(StandardCursorType.TopRightCorner),
        Edge.Left or Edge.Right => new Cursor(StandardCursorType.SizeWestEast),
        Edge.Top or Edge.Bottom => new Cursor(StandardCursorType.SizeNorthSouth),
        Edge.Move => new Cursor(StandardCursorType.SizeAll),
        Edge.New => new Cursor(StandardCursorType.Cross),
        _ => Cursor.Default,
    };

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            return;
        }

        if (!_editingCrop)
        {
            if (_bitmap != null)
            {
                Clicked?.Invoke(this, EventArgs.Empty);
                e.Handled = true;
            }

            return;
        }

        Point p = e.GetPosition(this);
        _drag = HitCrop(p);
        _dragStart = p;
        _dragOrigin = _crop ?? new CropRect(0, 0, _sourceWidth, _sourceHeight);
        if (_drag == Edge.New)
        {
            Point s = ToSource(p);
            _dragOrigin = new CropRect((int)Math.Clamp(s.X, 0, _sourceWidth), (int)Math.Clamp(s.Y, 0, _sourceHeight), 0, 0);
        }

        e.Pointer.Capture(this);
        e.Handled = true;
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        if (!_editingCrop)
        {
            Cursor = _bitmap != null ? new Cursor(StandardCursorType.Hand) : Cursor.Default;
            return;
        }

        Point p = e.GetPosition(this);
        if (_drag == Edge.None)
        {
            Cursor = CursorFor(HitCrop(p));
            return;
        }

        double scale = ViewScale;
        if (scale <= 0)
        {
            return;
        }

        double dx = (p.X - _dragStart.X) / scale;
        double dy = (p.Y - _dragStart.Y) / scale;
        _crop = _drag == Edge.New ? NewRect(ToSource(p)) : Adjust(_dragOrigin, _drag, dx, dy);
        InvalidateVisual();
    }

    private CropRect NewRect(Point current)
    {
        double x0 = _dragOrigin.X;
        double y0 = _dragOrigin.Y;
        double x1 = Math.Clamp(current.X, 0, _sourceWidth);
        double y1 = Math.Clamp(current.Y, 0, _sourceHeight);
        int left = (int)Math.Min(x0, x1);
        int top = (int)Math.Min(y0, y1);
        int width = Math.Max((int)Math.Abs(x1 - x0), MinCropPixels);
        int height = Math.Max((int)Math.Abs(y1 - y0), MinCropPixels);
        return Clamp(new CropRect(left, top, width, height));
    }

    private CropRect Adjust(CropRect origin, Edge edge, double dx, double dy)
    {
        double left = origin.X;
        double top = origin.Y;
        double right = origin.X + origin.Width;
        double bottom = origin.Y + origin.Height;

        if (edge == Edge.Move)
        {
            double moveX = Math.Clamp(dx, -left, _sourceWidth - right);
            double moveY = Math.Clamp(dy, -top, _sourceHeight - bottom);
            return new CropRect((int)Math.Round(left + moveX), (int)Math.Round(top + moveY), origin.Width, origin.Height);
        }

        if (edge.HasFlag(Edge.Left))
        {
            left = Math.Clamp(left + dx, 0, right - MinCropPixels);
        }

        if (edge.HasFlag(Edge.Right))
        {
            right = Math.Clamp(right + dx, left + MinCropPixels, _sourceWidth);
        }

        if (edge.HasFlag(Edge.Top))
        {
            top = Math.Clamp(top + dy, 0, bottom - MinCropPixels);
        }

        if (edge.HasFlag(Edge.Bottom))
        {
            bottom = Math.Clamp(bottom + dy, top + MinCropPixels, _sourceHeight);
        }

        return new CropRect((int)Math.Round(left), (int)Math.Round(top), (int)Math.Round(right - left), (int)Math.Round(bottom - top));
    }

    private CropRect Clamp(CropRect crop)
    {
        int x = Math.Clamp(crop.X, 0, Math.Max(0, _sourceWidth - MinCropPixels));
        int y = Math.Clamp(crop.Y, 0, Math.Max(0, _sourceHeight - MinCropPixels));
        return new CropRect(x, y, Math.Min(crop.Width, _sourceWidth - x), Math.Min(crop.Height, _sourceHeight - y));
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        if (_drag != Edge.None)
        {
            _drag = Edge.None;
            e.Pointer.Capture(null);
            CropChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e)
    {
        base.OnPointerCaptureLost(e);
        _drag = Edge.None;
    }
}
