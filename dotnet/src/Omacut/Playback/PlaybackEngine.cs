using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using Omacut.Core;

namespace Omacut.Playback;

/// <summary>A decoded preview frame (BGRA, premultiplied opaque) and its media time.</summary>
internal sealed class VideoFrame(byte[] pixels, double time)
{
    public byte[] Pixels { get; } = pixels;
    public double Time { get; } = time;
}

/// <summary>
/// ffmpeg-backed preview playback. Video frames are decoded at preview size and a fixed rate
/// into a bounded queue; audio streams through OpenAL and is the master clock. The UI polls
/// <see cref="TakeDueFrame"/> on its render tick and draws the newest frame that is due.
/// </summary>
internal sealed class PlaybackEngine : IDisposable
{
    private const int QueueCapacity = 8;
    private const int MaxPreviewLongSide = 1280;
    private const int MaxPreviewFps = 60;

    private readonly FfmpegTools _tools;
    private readonly AudioOutput? _audio;
    private readonly object _gate = new();
    private readonly SemaphoreSlim _grabLock = new(1, 1);

    private MediaInfo? _media;
    private Session? _session;
    private double _pausedPosition;
    private double _pendingGrab = double.NaN;
    private bool _muted;
    private bool _disposed;

    public PlaybackEngine(FfmpegTools tools)
    {
        _tools = tools;
        _audio = AudioOutput.TryCreate(out string? error);
        AudioError = error;
    }

    public string? AudioError { get; }
    public bool HasAudioOutput => _audio != null;
    public int FrameWidth { get; private set; }
    public int FrameHeight { get; private set; }
    public double PlaybackFps { get; private set; } = 30;
    public int FrameBytes => FrameWidth * FrameHeight * 4;

    public bool IsPlaying
    {
        get
        {
            lock (_gate)
            {
                return _session != null;
            }
        }
    }

    public bool Muted
    {
        get => _muted;
        set
        {
            _muted = value;
            if (_audio != null)
            {
                _audio.Volume = value ? 0f : 1f;
            }
        }
    }

    /// <summary>Current media time: the playback clock while playing, else the paused position.</summary>
    public double Position
    {
        get
        {
            lock (_gate)
            {
                return _session?.Clock() ?? _pausedPosition;
            }
        }
    }

    public void Load(MediaInfo media)
    {
        Stop();
        _media = media;
        (FrameWidth, FrameHeight) = PreviewSize(media.Width, media.Height);
        PlaybackFps = Math.Min(media.FrameRate > 0 ? media.FrameRate : 30, MaxPreviewFps);
        _pausedPosition = 0;
    }

    internal static (int Width, int Height) PreviewSize(int width, int height)
    {
        double scale = Math.Min(1.0, MaxPreviewLongSide / (double)Math.Max(width, height));
        int w = Math.Max(2, (int)Math.Round(width * scale / 2) * 2);
        int h = Math.Max(2, (int)Math.Round(height * scale / 2) * 2);
        return (w, h);
    }

    /// <summary>Starts playback at <paramref name="from"/>; it stops by itself at <paramref name="until"/>.</summary>
    public void Play(double from, double until)
    {
        if (_media is not { } media || _disposed)
        {
            return;
        }

        Stop();
        var session = new Session(this, media, from, until);
        lock (_gate)
        {
            _session = session;
        }

        session.Start();
    }

    /// <summary>Stops playback and remembers where it was.</summary>
    public double Pause()
    {
        Session? session;
        lock (_gate)
        {
            session = _session;
            _session = null;
            if (session != null)
            {
                _pausedPosition = Math.Min(session.Clock(), session.Until);
            }
        }

        session?.Dispose();
        return _pausedPosition;
    }

    private void Stop()
    {
        Session? session;
        lock (_gate)
        {
            session = _session;
            _session = null;
        }

        session?.Dispose();
    }

    /// <summary>
    /// Returns the newest decoded frame whose time has come, or null. The caller must hand the
    /// frame back through <see cref="Return"/>. Sets <paramref name="ended"/> once the clip end
    /// (or the end of the media) has been reached, after which the caller should pause.
    /// </summary>
    public VideoFrame? TakeDueFrame(out bool ended)
    {
        ended = false;
        Session? session;
        lock (_gate)
        {
            session = _session;
        }

        return session?.TakeDue(out ended);
    }

    public void Return(VideoFrame frame) => ArrayPool<byte>.Shared.Return(frame.Pixels);

    /// <summary>
    /// Grabs the frame at <paramref name="time"/> for the paused preview. Requests coalesce: while
    /// one grab runs, only the most recent request is kept, so scrubbing never queues a backlog.
    /// Returns null when this request was superseded or failed.
    /// </summary>
    public async Task<VideoFrame?> GrabFrameAsync(double time)
    {
        if (_media is not { } media || !_tools.HasFfmpeg)
        {
            return null;
        }

        lock (_gate)
        {
            _pausedPosition = time;
        }

        Volatile.Write(ref _pendingGrab, time);
        await _grabLock.WaitAsync().ConfigureAwait(false);
        try
        {
            double target = Volatile.Read(ref _pendingGrab);
            if (double.IsNaN(target) || target != time)
            {
                return null;
            }

            Volatile.Write(ref _pendingGrab, double.NaN);
            byte[]? pixels = await DecodeSingleFrameAsync(media, target).ConfigureAwait(false);
            return pixels == null ? null : new VideoFrame(pixels, target);
        }
        finally
        {
            _grabLock.Release();
        }
    }

    private async Task<byte[]?> DecodeSingleFrameAsync(MediaInfo media, double time)
    {
        // Near the very end a seek can land past the last frame and yield nothing, so back off
        // by one frame when that happens.
        foreach (double candidate in new[] { time, Math.Max(0, time - media.FrameDuration), Math.Max(0, time - 0.5) })
        {
            using Process process = FfmpegTools.StartProcess(_tools.FfmpegPath, SingleFrameArgs(media.Path, candidate));
            Task<string> stderr = process.StandardError.ReadToEndAsync();
            byte[] pixels = ArrayPool<byte>.Shared.Rent(FrameBytes);
            int total = 0;
            try
            {
                Stream stdout = process.StandardOutput.BaseStream;
                while (total < FrameBytes)
                {
                    int read = await stdout.ReadAsync(pixels.AsMemory(total, FrameBytes - total)).ConfigureAwait(false);
                    if (read == 0)
                    {
                        break;
                    }

                    total += read;
                }
            }
            finally
            {
                FfmpegTools.KillQuietly(process);
                await process.WaitForExitAsync().ConfigureAwait(false);
                await stderr.ConfigureAwait(false);
            }

            if (total == FrameBytes)
            {
                return pixels;
            }

            ArrayPool<byte>.Shared.Return(pixels);
        }

        return null;
    }

    private List<string> SingleFrameArgs(string path, double time) =>
    [
        "-hide_banner", "-loglevel", "error",
        "-ss", TimeFormat.Ffmpeg(time), "-i", path,
        "-an", "-sn", "-frames:v", "1",
        "-vf", ScaleFilter(),
        "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1",
    ];

    private List<string> VideoStreamArgs(string path, double from) =>
    [
        "-hide_banner", "-loglevel", "error",
        "-ss", TimeFormat.Ffmpeg(from), "-i", path,
        "-an", "-sn",
        "-vf", "fps=" + PlaybackFps.ToString("0.###", CultureInfo.InvariantCulture) + "," + ScaleFilter(),
        "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1",
    ];

    private static List<string> AudioStreamArgs(string path, double from) =>
    [
        "-hide_banner", "-loglevel", "error",
        "-ss", TimeFormat.Ffmpeg(from), "-i", path,
        "-vn", "-sn",
        "-ac", AudioOutput.Channels.ToString(CultureInfo.InvariantCulture),
        "-ar", AudioOutput.SampleRate.ToString(CultureInfo.InvariantCulture),
        "-f", "s16le", "pipe:1",
    ];

    private string ScaleFilter() => string.Create(CultureInfo.InvariantCulture, $"scale={FrameWidth}:{FrameHeight}:flags=bilinear");

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        Stop();
        _audio?.Dispose();
        _grabLock.Dispose();
    }

    /// <summary>One play run: a video decoder, an optional audio decoder, and the clock.</summary>
    private sealed class Session : IDisposable
    {
        private readonly PlaybackEngine _engine;
        private readonly MediaInfo _media;
        private readonly CancellationTokenSource _cancel = new();
        private readonly BlockingCollection<VideoFrame> _frames = new(QueueCapacity);
        private readonly ManualResetEventSlim _audioPrebuffered = new(false);
        private readonly ManualResetEventSlim _clockStarted = new(false);
        private readonly Stopwatch _wall = new();
        private readonly List<Process> _processes = [];
        private VideoFrame? _held;
        private volatile bool _videoEnded;
        private volatile bool _audioActive;
        private double _audioEndOffset = double.NaN;
        private Thread? _videoThread;
        private Thread? _audioThread;

        public Session(PlaybackEngine engine, MediaInfo media, double from, double until)
        {
            _engine = engine;
            _media = media;
            From = from;
            Until = Math.Min(until, media.Duration);
        }

        public double From { get; }
        public double Until { get; }

        public void Start()
        {
            bool withAudio = _engine._audio != null && _media.HasAudio;
            _videoThread = new Thread(RunVideo) { IsBackground = true, Name = "omacut-video" };
            _videoThread.Start();

            if (withAudio)
            {
                _audioActive = true;
                _audioThread = new Thread(RunAudio) { IsBackground = true, Name = "omacut-audio" };
                _audioThread.Start();
            }
            else
            {
                _audioPrebuffered.Set();
            }

            // Start the clock once the first frame and the audio prebuffer are ready, so the
            // picture and sound leave the gate together.
            CancellationToken token = _cancel.Token;
            _ = Task.Run(() =>
            {
                try
                {
                    var deadline = Stopwatch.StartNew();
                    while (!token.IsCancellationRequested && _frames.Count == 0 && !_videoEnded && deadline.ElapsedMilliseconds < 3000)
                    {
                        Thread.Sleep(2);
                    }

                    _audioPrebuffered.Wait(TimeSpan.FromSeconds(3), token);
                    _wall.Start();
                    _clockStarted.Set();
                }
                catch (Exception ex) when (ex is OperationCanceledException or ObjectDisposedException)
                {
                    // The session was paused before playback got going.
                }
            }, token);
        }

        public double Clock()
        {
            if (!_clockStarted.IsSet)
            {
                return From;
            }

            double wall = _wall.Elapsed.TotalSeconds;
            if (_audioActive && _engine._audio is { } audio)
            {
                double audioTime = audio.PlayedSeconds;

                // The audio clock only advances once the device starts; fall back to the wall
                // clock during that brief start-up and after the audio track ends.
                if (audioTime > 0)
                {
                    return From + audioTime;
                }
            }
            else if (!double.IsNaN(_audioEndOffset))
            {
                return From + Math.Max(wall, _audioEndOffset);
            }

            return From + wall;
        }

        public VideoFrame? TakeDue(out bool ended)
        {
            double clock = Clock();
            ended = clock >= Until || (_videoEnded && _frames.Count == 0 && _held == null && (!_audioActive || clock >= _media.Duration));

            VideoFrame? due = null;
            while (true)
            {
                VideoFrame? next = _held;
                _held = null;
                if (next == null && !_frames.TryTake(out next))
                {
                    break;
                }

                if (next.Time > clock + 0.001)
                {
                    _held = next; // not due yet; keep it for a later tick
                    break;
                }

                if (due != null)
                {
                    _engine.Return(due); // dropped: a newer frame is also due
                }

                due = next;
            }

            return due;
        }

        private void RunVideo()
        {
            CancellationToken token = _cancel.Token;
            try
            {
                using Process process = FfmpegTools.StartProcess(_engine._tools.FfmpegPath, _engine.VideoStreamArgs(_media.Path, From));
                lock (_processes)
                {
                    _processes.Add(process);
                }

                _ = process.StandardError.ReadToEndAsync(token);
                Stream stdout = process.StandardOutput.BaseStream;
                int frameBytes = _engine.FrameBytes;
                double step = 1.0 / _engine.PlaybackFps;
                long index = 0;

                while (!token.IsCancellationRequested)
                {
                    byte[] pixels = ArrayPool<byte>.Shared.Rent(frameBytes);
                    int total = 0;
                    while (total < frameBytes)
                    {
                        int read = stdout.Read(pixels, total, frameBytes - total);
                        if (read == 0)
                        {
                            break;
                        }

                        total += read;
                    }

                    if (total < frameBytes)
                    {
                        ArrayPool<byte>.Shared.Return(pixels);
                        break;
                    }

                    double time = From + index++ * step;
                    var frame = new VideoFrame(pixels, time);
                    try
                    {
                        _frames.Add(frame, token);
                    }
                    catch (OperationCanceledException)
                    {
                        ArrayPool<byte>.Shared.Return(pixels);
                        throw;
                    }

                    if (time >= Until)
                    {
                        break;
                    }
                }
            }
            catch (Exception ex) when (ex is OperationCanceledException or IOException or ObjectDisposedException or InvalidOperationException or System.ComponentModel.Win32Exception)
            {
                // Pause/seek killed the decoder, or it could not start; either way this run is over.
            }
            finally
            {
                _videoEnded = true;
            }
        }

        private void RunAudio()
        {
            CancellationToken token = _cancel.Token;
            try
            {
                using Process process = FfmpegTools.StartProcess(_engine._tools.FfmpegPath, AudioStreamArgs(_media.Path, From));
                lock (_processes)
                {
                    _processes.Add(process);
                }

                _ = process.StandardError.ReadToEndAsync(token);
                bool finished = _engine._audio!.Stream(process.StandardOutput.BaseStream, _audioPrebuffered, _clockStarted, token);
                if (finished)
                {
                    // Audio shorter than the video: keep time flowing on the wall clock.
                    _audioEndOffset = _engine._audio.PlayedSeconds;
                }
            }
            catch (Exception ex) when (ex is OperationCanceledException or IOException or ObjectDisposedException or InvalidOperationException or System.ComponentModel.Win32Exception)
            {
            }
            finally
            {
                _audioPrebuffered.Set();
                _audioActive = false;
            }
        }

        public void Dispose()
        {
            _cancel.Cancel();
            _clockStarted.Set(); // release the audio thread if it is still waiting at the gate
            lock (_processes)
            {
                foreach (Process process in _processes)
                {
                    FfmpegTools.KillQuietly(process);
                }
            }

            _videoThread?.Join(2000);
            _audioThread?.Join(2000);

            if (_held != null)
            {
                _engine.Return(_held);
                _held = null;
            }

            while (_frames.TryTake(out VideoFrame? frame))
            {
                _engine.Return(frame);
            }

            _frames.Dispose();
            _cancel.Dispose();
            _audioPrebuffered.Dispose();
            _clockStarted.Dispose();
        }
    }
}
