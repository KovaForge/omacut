# Omacut for .NET (Avalonia)

A native C#/Avalonia port of [omacut](https://github.com/omacom/omacut), the dead-simple video trimmer, built to be embedded in host apps such as [XerahS](https://github.com/KovaForge/XerahS). It also ships as a standalone `omacut` app.

Open a video, drag the two handles to pick a start and end, preview the clip, and export. On Omarchy the interface follows your theme's accent color live.

## What carries over from omacut

- Filmstrip trim bar: two handles, a scrubbable playhead, the floating time bubble while trimming, and dimming outside the selection.
- `Z` zooms into the selection for fine-tuning. The full-length filmstrip is cached, so zooming back out is instant.
- The same hotkeys, and the playhead stays inside the trim.
- The opening frame shows immediately on load, never a black box.
- Quitting asks first only when the trim hasn't been exported.
- Exports are transactional: ffmpeg writes to a sibling temp file, which replaces the target only on success. Progress comes from `-progress`.
- MP4 output with Original/1080p/720p choices, never upscaling and always keeping the aspect ratio.

## What's new

- **Crop on the preview** (`C`): drag a rectangle or its edges and corners. `Enter` applies, `Esc` cancels, `Backspace` resets. The applied crop is what the preview shows.
- **Formats:** MP4 (H.264/AAC), WebM (VP9/Opus), or GIF (palette-optimized). Encoders are probed, so each ffmpeg build falls back to codecs it actually has.
- **Audio:** keep or remove it on export. `M` mutes playback.
- **Frame stepping** with `,` and `.`; `I`/`O` set the trim start and end; `Home`/`End` jump to the trim edges.
- **Cancellable export** with a progress bar.
- **Drag and drop** a video onto the window.
- **Host API:** `OmacutEditor.ShowAsync(options)` returns the exported path. Hosts can also pass ffmpeg paths, an accent color, a PNG watermark to burn in, a default output folder, and close-after-export.

## Playback

Preview playback is native and needs no WebView or libvlc:

- ffmpeg decodes preview-sized BGRA frames into an Avalonia `WriteableBitmap`.
- Audio streams as PCM into OpenAL. OpenAL Soft natives for Windows, Linux and macOS come from `Silk.NET.OpenAL.Soft.Native`.
- The audio clock drives A/V sync.
- Paused scrubbing grabs single frames and coalesces requests, so dragging never builds a backlog.

Anything ffmpeg can read, the editor can play. When no audio device is available, playback continues muted.

## Hotkeys

| Keys | Action |
|---|---|
| `Space` | Play / pause |
| `←` / `→` | Move playhead 1 s (`Shift` 5 s, `Alt` 0.2 s) |
| `,` / `.` | Previous / next frame |
| `Ctrl+Space` or `I` | Trim start to playhead |
| `Alt+Space` or `O` | Trim end to playhead |
| `Home` / `End` | Playhead to trim start / end |
| `Z` | Zoom the selection |
| `C` | Crop |
| `M` | Mute |
| `Ctrl+O` | Open a video |
| `Ctrl+S` | Export |
| `Q` | Quit (asks first if the trim hasn't been exported) |
| `?` | Show the hotkeys |

## Requirements

- .NET 10
- `ffmpeg` and `ffprobe`, either on `PATH` or passed by the host

## Build, test, run

```bash
cd dotnet
dotnet build Omacut.sln
dotnet test tests/Omacut.Tests/Omacut.Tests.csproj   # real ffmpeg integration + headless UI tests
dotnet run --project src/Omacut.App -- path/to/video.mp4
```

## Embedding

```csharp
string? exported = await OmacutEditor.ShowAsync(new OmacutEditorOptions
{
    VideoPath = path,
    FfmpegPath = ffmpeg,
    WindowTitle = "MyApp",
    CloseAfterExport = true,
}, owner: mainWindow);
```

`EditorView` is a plain control if you'd rather host it yourself.

**Single-file hosts:** keep the OpenAL Soft library (`libopenal.so`, `libopenal.dylib`, or `soft_oal.dll`) next to your executable rather than inside the bundle. Silk.NET's loader doesn't look in the bundle's extraction folder, so a bundled copy is never found and audio falls back to the system OpenAL, which Windows doesn't have. The fix in the host project:

```xml
<Target Name="KeepOpenAlOutOfSingleFileBundle" AfterTargets="ComputeResolvedFilesToPublishList">
  <ItemGroup>
    <ResolvedFileToPublish Update="@(ResolvedFileToPublish)"
                           Condition="'%(Filename)%(Extension)' == 'libopenal.so' Or '%(Filename)%(Extension)' == 'libopenal.dylib' Or '%(Filename)%(Extension)' == 'soft_oal.dll'"
                           ExcludeFromSingleFile="true" />
  </ItemGroup>
</Target>
```

If no OpenAL can be loaded at all, the editor still works; playback is just muted, and the mute button explains why.

## Layout

- `src/Omacut` (`Omacut.Editor.dll`): the core, playback, controls and hosting API.
- `src/Omacut.App`: the standalone `omacut` executable.
- `tests/Omacut.Tests`: unit, ffmpeg integration and headless UI tests.

The original Qt/QML implementation stays at the repository root. The fork's `master` branch tracks upstream omacut, and upstream changes are ported into `dotnet/` semantically.

## License

MIT, like omacut. See `../LICENSE`.
