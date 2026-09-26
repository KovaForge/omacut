using Silk.NET.OpenAL;

namespace Omacut.Playback;

/// <summary>
/// A streaming OpenAL source fed with 48 kHz stereo s16 PCM. One instance per editor.
/// OpenAL Soft natives ship with the package for Windows, Linux and macOS.
/// </summary>
internal sealed unsafe class AudioOutput : IDisposable
{
    public const int SampleRate = 48000;
    public const int Channels = 2;
    public const int BytesPerFrame = Channels * sizeof(short);

    private const int BufferCount = 6;
    private const int FramesPerBuffer = 2048; // ~43 ms each, ~256 ms queued

    private readonly object _gate = new();
    private readonly AL _al;
    private readonly ALContext _alc;
    private readonly Device* _device;
    private readonly Context* _context;
    private readonly uint _source;
    private readonly uint[] _buffers;
    private readonly Queue<(uint Buffer, int Frames)> _queued = new();
    private long _playedFrames;
    private bool _disposed;

    private AudioOutput(AL al, ALContext alc, Device* device, Context* context)
    {
        _al = al;
        _alc = alc;
        _device = device;
        _context = context;
        _source = _al.GenSource();
        _buffers = _al.GenBuffers(BufferCount);
    }

    public static AudioOutput? TryCreate(out string? error)
    {
        error = null;
        try
        {
            ALContext alc = ALContext.GetApi(soft: true);
            AL al = AL.GetApi(soft: true);
            Device* device = alc.OpenDevice(string.Empty);
            if (device == null)
            {
                error = "No audio output device.";
                return null;
            }

            Context* context = alc.CreateContext(device, null);
            if (context == null || !alc.MakeContextCurrent(context))
            {
                alc.CloseDevice(device);
                error = "Could not create an audio context.";
                return null;
            }

            return new AudioOutput(al, alc, device, context);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or FileNotFoundException or InvalidOperationException)
        {
            error = "Audio is unavailable: " + ex.Message;
            return null;
        }
    }

    /// <summary>Frames that have finished playing since <see cref="Reset"/>, plus the source's current offset.</summary>
    public double PlayedSeconds
    {
        get
        {
            lock (_gate)
            {
                if (_disposed)
                {
                    return 0;
                }

                _al.GetSourceProperty(_source, GetSourceInteger.SampleOffset, out int offset);
                return (_playedFrames + Math.Max(offset, 0)) / (double)SampleRate;
            }
        }
    }

    public float Volume
    {
        set
        {
            lock (_gate)
            {
                if (!_disposed)
                {
                    _al.SetSourceProperty(_source, SourceFloat.Gain, Math.Clamp(value, 0f, 1f));
                }
            }
        }
    }

    public void Reset()
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            _al.SourceStop(_source);
            _al.SetSourceProperty(_source, SourceInteger.Buffer, 0u);
            _queued.Clear();
            _playedFrames = 0;
        }
    }

    /// <summary>
    /// Streams PCM until the stream ends or the token fires. <paramref name="prebuffered"/> is set
    /// once the first buffers are queued; playback starts when <paramref name="start"/> is set.
    /// Returns true when the stream ran to its end.
    /// </summary>
    public bool Stream(Stream pcm, ManualResetEventSlim prebuffered, ManualResetEventSlim start, CancellationToken cancellationToken)
    {
        Reset();
        byte[] chunk = new byte[FramesPerBuffer * BytesPerFrame];
        var free = new Queue<uint>(_buffers);
        bool endOfStream = false;

        try
        {
            while (free.Count > 0 && !endOfStream)
            {
                int read = ReadFull(pcm, chunk, cancellationToken);
                endOfStream = read < chunk.Length;
                if (read > 0)
                {
                    Enqueue(free.Dequeue(), chunk, read);
                }
            }

            prebuffered.Set();
            start.Wait(cancellationToken);

            lock (_gate)
            {
                if (_queued.Count > 0)
                {
                    _al.SourcePlay(_source);
                }
            }

            while (!cancellationToken.IsCancellationRequested)
            {
                int processed;
                lock (_gate)
                {
                    _al.GetSourceProperty(_source, GetSourceInteger.BuffersProcessed, out processed);
                    for (int i = 0; i < processed; i++)
                    {
                        (uint buffer, int frames) = _queued.Dequeue();
                        uint handle = buffer;
                        _al.SourceUnqueueBuffers(_source, 1, &handle);
                        _playedFrames += frames;
                        free.Enqueue(buffer);
                    }
                }

                while (free.Count > 0 && !endOfStream)
                {
                    int read = ReadFull(pcm, chunk, cancellationToken);
                    endOfStream = read < chunk.Length;
                    if (read > 0)
                    {
                        Enqueue(free.Dequeue(), chunk, read);
                    }
                }

                lock (_gate)
                {
                    _al.GetSourceProperty(_source, GetSourceInteger.SourceState, out int state);
                    if ((SourceState)state != SourceState.Playing)
                    {
                        if (_queued.Count == 0 && endOfStream)
                        {
                            return true;
                        }

                        // Underrun: the decoder fell behind; resume as soon as data is queued.
                        if (_queued.Count > 0)
                        {
                            _al.SourcePlay(_source);
                        }
                    }
                }

                cancellationToken.WaitHandle.WaitOne(10);
            }

            return false;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
        catch (IOException)
        {
            // The decoder was killed (pause/seek) while we were reading.
            return false;
        }
        finally
        {
            prebuffered.Set();
            lock (_gate)
            {
                if (!_disposed)
                {
                    _al.SourcePause(_source);
                }
            }
        }
    }

    private void Enqueue(uint buffer, byte[] data, int length)
    {
        int usable = length - (length % BytesPerFrame);
        if (usable <= 0)
        {
            return;
        }

        lock (_gate)
        {
            fixed (byte* pointer = data)
            {
                _al.BufferData(buffer, BufferFormat.Stereo16, pointer, usable, SampleRate);
            }

            uint handle = buffer;
            _al.SourceQueueBuffers(_source, 1, &handle);
            _queued.Enqueue((buffer, usable / BytesPerFrame));
        }
    }

    private static int ReadFull(Stream stream, byte[] buffer, CancellationToken cancellationToken)
    {
        int total = 0;
        while (total < buffer.Length)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int read = stream.Read(buffer, total, buffer.Length - total);
            if (read == 0)
            {
                break;
            }

            total += read;
        }

        return total;
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _al.SourceStop(_source);
            _al.SetSourceProperty(_source, SourceInteger.Buffer, 0u);
            _al.DeleteSource(_source);
            _al.DeleteBuffers(_buffers);
            _alc.MakeContextCurrent(null);
            _alc.DestroyContext(_context);
            _alc.CloseDevice(_device);
            _al.Dispose();
            _alc.Dispose();
        }
    }
}
