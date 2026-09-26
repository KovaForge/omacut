using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Immutable;

namespace Omacut.Controls;

/// <summary>A thin rounded progress bar in the accent color.</summary>
internal sealed class ProgressTrack : Control
{
    private double _value;

    public ProgressTrack()
    {
        Height = 6;
    }

    public Color Accent { get; set; } = Colors.Gold;

    public double Value
    {
        get => _value;
        set
        {
            _value = Math.Clamp(value, 0, 1);
            InvalidateVisual();
        }
    }

    public override void Render(DrawingContext context)
    {
        var track = new Rect(Bounds.Size);
        double radius = track.Height / 2;
        context.DrawRectangle(Palette.SurfaceBrush, null, new RoundedRect(track, radius));
        if (_value > 0)
        {
            context.DrawRectangle(new ImmutableSolidColorBrush(Accent), null, new RoundedRect(new Rect(0, 0, Math.Max(track.Height, track.Width * _value), track.Height), radius));
        }
    }
}
