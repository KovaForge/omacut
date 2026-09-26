using System.Globalization;

namespace Omacut.Core;

/// <summary>
/// Reads the accent color from the current Omarchy theme and follows theme switches live.
/// On systems without Omarchy the fallback accent is used, which keeps omacut working anywhere.
/// </summary>
public sealed class OmarchyTheme : IDisposable
{
    public const string DefaultAccent = "#FFD60A";

    private readonly string _fallback;
    private readonly List<FileSystemWatcher> _watchers = [];
    private readonly Timer _debounce;

    public OmarchyTheme(string? fallback = null)
    {
        _fallback = IsColor(fallback) ? fallback! : DefaultAccent;
        Accent = AccentFromColorsFile(ColorsPath, _fallback);
        _debounce = new Timer(_ => Reload(), null, Timeout.Infinite, Timeout.Infinite);
        Watch();
    }

    public string Accent { get; private set; }

    /// <summary>Raised on a thread-pool thread when the accent changes.</summary>
    public event EventHandler? AccentChanged;

    public static string CurrentDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "state", "omarchy", "current");

    public static string ColorsPath => Path.Combine(CurrentDirectory, "theme", "colors.toml");

    public static string AccentFromColorsFile(string path, string fallback)
    {
        string[] lines;
        try
        {
            if (!File.Exists(path))
            {
                return fallback;
            }

            lines = File.ReadAllLines(path);
        }
        catch (IOException)
        {
            return fallback;
        }
        catch (UnauthorizedAccessException)
        {
            return fallback;
        }

        foreach (string raw in lines)
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#'))
            {
                continue;
            }

            int equals = line.IndexOf('=');
            if (equals < 0 || line[..equals].Trim() != "accent")
            {
                continue;
            }

            string value = line[(equals + 1)..].Trim();
            if (value.Length >= 2 && ((value[0] == '"' && value[^1] == '"') || (value[0] == '\'' && value[^1] == '\'')))
            {
                value = value[1..^1];
            }

            return IsColor(value) ? value : fallback;
        }

        return fallback;
    }

    /// <summary>"#000000" or "#FFFFFF", whichever stays legible on the given color.</summary>
    public static string ForegroundFor(string color)
    {
        if (!TryParseRgb(color, out double r, out double g, out double b))
        {
            return "#000000";
        }

        double luminance = 0.299 * r + 0.587 * g + 0.114 * b;
        return luminance < 0.5 ? "#FFFFFF" : "#000000";
    }

    internal static bool IsColor(string? value) => value != null && TryParseRgb(value, out _, out _, out _);

    private static bool TryParseRgb(string value, out double r, out double g, out double b)
    {
        r = g = b = 0;
        string hex = value.Trim().TrimStart('#');
        if (hex.Length == 3)
        {
            hex = string.Concat(hex.Select(c => new string(c, 2)));
        }
        else if (hex.Length == 8)
        {
            hex = hex[2..];
        }

        if (hex.Length != 6 || !int.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int rgb))
        {
            return false;
        }

        r = ((rgb >> 16) & 0xFF) / 255.0;
        g = ((rgb >> 8) & 0xFF) / 255.0;
        b = (rgb & 0xFF) / 255.0;
        return true;
    }

    // The theme lives behind a symlink that gets swapped, so every reload re-arms the watches.
    private void Watch()
    {
        foreach (FileSystemWatcher watcher in _watchers)
        {
            watcher.Dispose();
        }

        _watchers.Clear();
        foreach (string directory in new[] { CurrentDirectory, Path.Combine(CurrentDirectory, "theme") })
        {
            try
            {
                if (!Directory.Exists(directory))
                {
                    continue;
                }

                var watcher = new FileSystemWatcher(directory)
                {
                    NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.CreationTime,
                    IncludeSubdirectories = false,
                };
                watcher.Changed += OnChanged;
                watcher.Created += OnChanged;
                watcher.Deleted += OnChanged;
                watcher.Renamed += OnChanged;
                watcher.EnableRaisingEvents = true;
                _watchers.Add(watcher);
            }
            catch (Exception ex) when (ex is IOException or ArgumentException or UnauthorizedAccessException or PlatformNotSupportedException)
            {
                // Watching is best effort; the accent still loads at startup.
            }
        }
    }

    private void OnChanged(object sender, FileSystemEventArgs e) => _debounce.Change(150, Timeout.Infinite);

    private void Reload()
    {
        lock (_watchers)
        {
            Watch();
        }

        string accent = AccentFromColorsFile(ColorsPath, _fallback);
        if (accent == Accent)
        {
            return;
        }

        Accent = accent;
        AccentChanged?.Invoke(this, EventArgs.Empty);
    }

    public void Dispose()
    {
        _debounce.Dispose();
        foreach (FileSystemWatcher watcher in _watchers)
        {
            watcher.Dispose();
        }

        _watchers.Clear();
    }
}
