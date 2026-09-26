# Omacut

A dead-simple video **length** trimmer. Open a video, drag the two handles to pick a start and end, preview the clip, and export. On Omarchy, the interface follows your theme's accent color.

Built using **Qt Quick (QML)** UI with the Material style — the same Qt stack Quickshell builds on — and **ffmpeg** for the cut. The C++ side compiles to a single executable; the QML is embedded in it via Qt resources.

<img width="3227" height="3227" alt="screenshot-2026-06-23_15-20-40" src="https://github.com/user-attachments/assets/c76047c8-618f-4c1c-91f9-e7024c4f953b" />

## Hotkeys

- *Space*: Start/stop video playback.
- *Left/Right*: Move the playhead by 1 second.
- *Shift+Left/Right*: Move the playhead by 5 seconds.
- *Alt+Left/Right*: Move the playhead by 0.2 seconds.
- *Ctrl+Space*: Move the start of the trim to the playhead.
- *Alt+Space*: Move the end of the trim to the playhead.
- *Z*: Zoom into the trimmed selection for fine tuning (Z again zooms back out).
- *Ctrl+O*: Open a new file to trim.
- *Ctrl+S*: Export the current trim.
- *Ctrl+U*: Upload the current trim and copy its link (Escape cancels a running upload).
- *Q*: Quit (asks first if the trim hasn't been exported).
- *?*: Show the hotkeys in the app.

## Install

Install via the Omarchy Package Repository via the `omacut` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Requirements

- `xdg-desktop-portal` and a portal backend for the file picker
- `ffmpeg` and `ffprobe` on your PATH (used at runtime)
- Optionally `wl-clipboard` (uploaded links outlive omacut), `libsecret` (host passwords in the keyring) and `curl` (FTP and SFTP hosts)

Exports are always written as MP4 files, regardless of the input video's container. The export dialog offers Original/1080p/720p quality — never upscaling, and always preserving the aspect ratio.

## Upload

*Ctrl+U* (or the upload button) encodes the trim just like an export and sends it to a host. Pick the host and quality with the arrow keys, press Enter, and the link lands on your clipboard when it's done. Three anonymous hosts are built in, no account needed:

- **Litterbox**: deleted after 72 hours, up to 1 GB (the default)
- **Catbox**: kept, up to 200 MB
- **Uguu**: deleted after 3 hours, up to 128 MiB

### Your own hosts

*Hosts…* in the upload dialog adds, edits and removes your own hosts:

- **Amazon S3 or compatible**: AWS S3, Cloudflare R2, Backblaze B2, Wasabi, MinIO and others. Set the endpoint and region, and choose a public URL, a custom domain or a signed link (valid for 7 days) for private buckets. Large files go up in parts.
- **Dropbox**: create an app at [dropbox.com/developers/apps](https://www.dropbox.com/developers/apps) with the `files.content.write` and `sharing.write` permissions, add `http://127.0.0.1:52475/oauth2/callback` as its redirect URI, enter its app key and click *Sign in with Dropbox*.
- **Nextcloud**: *Sign in with Nextcloud* grants an app password in the browser (or paste one). The clip gets a public share link, optionally expiring.
- **Immich**: adds the clip to your library with an API key, and shares a link to it.
- **XBackBone**: the upload token from your profile, for XBackBone 3 or the newer API.
- **FTP / SFTP**: SFTP, FTP or FTPS through `curl`. For SFTP, the server must already be in `~/.ssh/known_hosts`. Set the folder's web address so links point there.
- **Imgur**: anonymous uploads with your own client ID (videos up to 200 MB and 60 seconds).
- **Custom uploader (.sxcu)**: paste any ShareX custom uploader config.
- **Auto**: tries the hosts you pick, in order, and uses the first one that works.

Hosts are saved in `~/.config/omacut/hosts.json`. Passwords, keys and tokens never go there: they're kept in your desktop keyring through `secret-tool` (libsecret), or, without a keyring, in `~/.local/share/omacut/secrets.json`, readable only by you. Folder settings accept `%y`, `%mo` and `%d` for the date. Uploads to S3, Nextcloud and FTP get a short random tag in the name, so a new trim never replaces an old one.

ShareX custom uploader (`.sxcu`) files dropped in `~/.config/omacut/uploaders/` show up as hosts too, so configs from ShareX or XerahS work unchanged. Destinations with a multipart body (`FileFormName`) or a `Binary` body are supported, and responses are read with the `{response}`, `{json:path}`, `{regex:pattern|group}`, `{header:name}` and `{filename}` syntax. For example, `0x0.st.sxcu`:

```json
{
  "Name": "0x0.st",
  "RequestURL": "https://0x0.st",
  "Body": "MultipartFormData",
  "FileFormName": "file"
}
```

The last host you picked is remembered. Every upload is also appended to `~/.local/state/omacut/uploads.jsonl`, deletion links included. On Wayland the link is copied with `wl-copy` when it's installed, so it stays on the clipboard after omacut quits.

## Build

Uses Qt's own build tool, `qmake6` (no cmake needed):

```bash
./bin/build
```

This produces a single `omacut` binary in `build/`.

Requirements:

- A C++17 compiler and Qt6: `qt6-base` (including Qt Network), `qt6-declarative`
  (Qt Quick + Controls), `qt6-multimedia`

## Test

```bash
./bin/test
```

## Package

Build and install the local Arch package:

```bash
./bin/install
```

This runs `./bin/build`, then `makepkg -fsi` from `pkgbuild/` so same-version local packages are rebuilt and reinstalled. Extra arguments are passed through to `makepkg`, for example `./bin/install --clean`. The package installs the binary, desktop entry, app icon, and MIT license. Local package outputs such as `pkgbuild/pkg/`, `pkgbuild/src/`, and `*.pkg.tar.*` are ignored.

## License

MIT. See `LICENSE`.
