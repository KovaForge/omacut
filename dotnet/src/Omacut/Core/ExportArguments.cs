using System.Globalization;

namespace Omacut.Core;

/// <summary>Builds the ffmpeg command line for an export. Pure, so it is unit-tested directly.</summary>
public static class ExportArguments
{
    public static List<string> Build(ExportOptions options, MediaInfo info, EncoderSupport encoders, string outputPath)
    {
        string videoEncoder = encoders.VideoEncoder(options.Format)
            ?? throw new InvalidOperationException($"This ffmpeg build cannot encode {ExportFormats.DisplayName(options.Format)}.");

        // Machine-readable progress on stdout (errors stay on stderr). -ss before -i
        // seeks fast and, because we re-encode, is still frame-accurate; -t is the length.
        var args = new List<string> { "-y", "-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1" };
        args.AddRange(["-ss", TimeFormat.Ffmpeg(options.Start), "-i", options.SourcePath]);

        bool watermark = options.Watermark?.IsUsable == true && options.Format != ExportFormat.Gif;
        if (watermark)
        {
            args.AddRange(["-i", options.Watermark!.ImagePath]);
        }

        args.AddRange(["-t", TimeFormat.Ffmpeg(options.Length)]);

        string graph = BuildFilterGraph(options, info, watermark);
        if (graph.Length > 0)
        {
            args.AddRange(["-filter_complex", graph, "-map", "[vout]"]);
        }
        else
        {
            args.AddRange(["-map", "0:v:0"]);
        }

        string? audioEncoder = encoders.AudioEncoder(options.Format);
        bool audio = options.IncludeAudio && info.HasAudio && audioEncoder != null && options.Format != ExportFormat.Gif;
        if (audio)
        {
            args.AddRange(["-map", "0:a:0?"]);
        }
        else
        {
            args.Add("-an");
        }

        args.AddRange(["-sn", "-dn", "-map_metadata", "-1"]);

        switch (options.Format)
        {
            case ExportFormat.Mp4:
                args.AddRange(["-c:v", videoEncoder]);
                if (videoEncoder == "libx264")
                {
                    args.AddRange(["-preset", "veryfast", "-crf", "18"]);
                }

                args.AddRange(["-pix_fmt", "yuv420p"]);
                if (audio)
                {
                    args.AddRange(["-c:a", audioEncoder!, "-b:a", "192k"]);
                }

                // +faststart puts the moov atom up front so shared clips start playing
                // before they finish downloading.
                args.AddRange(["-movflags", "+faststart", "-f", "mp4"]);
                break;

            case ExportFormat.WebM:
                args.AddRange(["-c:v", videoEncoder]);
                if (videoEncoder == "libvpx-vp9")
                {
                    args.AddRange(["-crf", "32", "-b:v", "0", "-row-mt", "1", "-deadline", "good", "-cpu-used", "4"]);
                }
                else
                {
                    args.AddRange(["-crf", "10", "-b:v", "2M"]);
                }

                args.AddRange(["-pix_fmt", "yuv420p"]);
                if (audio)
                {
                    args.AddRange(["-c:a", audioEncoder!, "-b:a", "128k"]);
                }

                args.AddRange(["-f", "webm"]);
                break;

            case ExportFormat.Gif:
                args.AddRange(["-loop", "0", "-f", "gif"]);
                break;
        }

        args.Add(outputPath);
        return args;
    }

    internal static string BuildFilterGraph(ExportOptions options, MediaInfo info, bool watermark)
    {
        var chain = new List<string>();
        int width = info.Width;
        int height = info.Height;

        if (options.Crop is CropRect crop && !crop.IsEmpty && !crop.CoversFrame(info.Width, info.Height))
        {
            CropRect even = crop.Normalize(info.Width, info.Height);
            chain.Add(string.Create(CultureInfo.InvariantCulture, $"crop={even.Width}:{even.Height}:{even.X}:{even.Y}"));
            width = even.Width;
            height = even.Height;
        }

        if (options.ShortSide > 0 && Math.Min(width, height) > options.ShortSide)
        {
            // Cap the shorter side, judged on the decoded (rotation-applied) frame, so portrait
            // and landscape both keep their aspect ratio. -2 keeps the other side even.
            int size = options.ShortSide;
            chain.Add(string.Create(CultureInfo.InvariantCulture, $"scale='if(gt(iw,ih),-2,{size})':'if(gt(iw,ih),{size},-2)':flags=lanczos"));
        }
        else if (options.Format != ExportFormat.Gif && ((width & 1) != 0 || (height & 1) != 0))
        {
            chain.Add("scale=trunc(iw/2)*2:trunc(ih/2)*2");
        }

        if (options.Format == ExportFormat.Gif)
        {
            int fps = Math.Clamp(options.GifFrameRate, 1, 50);
            chain.Add(string.Create(CultureInfo.InvariantCulture, $"fps={fps}"));
            string head = chain.Count > 0 ? "[0:v]" + string.Join(',', chain) + "," : "[0:v]";
            return head + "split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle[vout]";
        }

        if (watermark)
        {
            WatermarkOverlay overlay = options.Watermark!;
            string opacity = Math.Clamp(overlay.Opacity, 0, 1).ToString("0.###", CultureInfo.InvariantCulture);
            string px = Math.Clamp(overlay.PositionX, 0, 1).ToString("0.###", CultureInfo.InvariantCulture);
            string py = Math.Clamp(overlay.PositionY, 0, 1).ToString("0.###", CultureInfo.InvariantCulture);
            string main = chain.Count > 0 ? "[0:v]" + string.Join(',', chain) + "[main];" : "[0:v]null[main];";
            return main
                + $"[1:v]format=rgba,colorchannelmixer=aa={opacity}[wm];"
                + $"[main][wm]overlay=x='(W-w)*{px}':y='(H-h)*{py}':format=auto:eof_action=repeat[vout]";
        }

        return chain.Count > 0 ? "[0:v]" + string.Join(',', chain) + "[vout]" : string.Empty;
    }
}
