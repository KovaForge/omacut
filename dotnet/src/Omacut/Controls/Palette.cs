using Avalonia.Media;
using Avalonia.Media.Immutable;

namespace Omacut.Controls;

/// <summary>omacut's dark palette; only the accent follows the theme.</summary>
internal static class Palette
{
    public static readonly Color Background = Color.Parse("#0e0e10");
    public static readonly Color Film = Color.Parse("#1c1c1e");
    public static readonly Color Surface = Color.Parse("#2c2c2f");
    public static readonly Color SurfaceHover = Color.Parse("#3a3a3e");
    public static readonly Color Text = Colors.White;
    public static readonly Color TextSecondary = Color.Parse("#d6d6da");
    public static readonly Color TextMuted = Color.Parse("#b8b8bc");
    public static readonly Color TextFaint = Color.Parse("#7a7a80");
    public static readonly Color Scrim = Color.Parse("#cc000000");
    public static readonly Color Dim = Color.Parse("#99000000");
    public static readonly Color Danger = Color.Parse("#ff6b6b");

    public static readonly IBrush BackgroundBrush = new ImmutableSolidColorBrush(Background);
    public static readonly IBrush FilmBrush = new ImmutableSolidColorBrush(Film);
    public static readonly IBrush SurfaceBrush = new ImmutableSolidColorBrush(Surface);
    public static readonly IBrush SurfaceHoverBrush = new ImmutableSolidColorBrush(SurfaceHover);
    public static readonly IBrush TextBrush = new ImmutableSolidColorBrush(Text);
    public static readonly IBrush TextSecondaryBrush = new ImmutableSolidColorBrush(TextSecondary);
    public static readonly IBrush TextMutedBrush = new ImmutableSolidColorBrush(TextMuted);
    public static readonly IBrush TextFaintBrush = new ImmutableSolidColorBrush(TextFaint);
    public static readonly IBrush ScrimBrush = new ImmutableSolidColorBrush(Scrim);
    public static readonly IBrush DimBrush = new ImmutableSolidColorBrush(Dim);
    public static readonly IBrush DangerBrush = new ImmutableSolidColorBrush(Danger);

    public static readonly FontFamily Mono = new("Cascadia Mono, Consolas, Menlo, JetBrainsMono Nerd Font, DejaVu Sans Mono, Liberation Mono, monospace");
}
