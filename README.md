# Skein Downloader

> **Work in progress.** Skein Downloader is under active development; expect missing
> features, rough edges and changes between releases.

Skein Downloader aims to be an open-source replacement for Free Download Manager:
multi-connection downloads with pause/resume, a download queue, and video downloads,
under the GPL. It is an independent project, not affiliated with or endorsed by the
makers of Free Download Manager.

A native C++20 desktop app for Windows and Linux built with wxWidgets 3.3.4. It runs
up to four downloads at a time with pause/resume, checksums and saved history; picks
files from Internet Archive collections, GitHub and GitLab releases, Hugging Face
repositories, file lockers and direct links; and downloads video through yt-dlp.

Windows uses wxMSW with wxWidgets and the C runtime linked statically. Linux uses wxGTK3,
which picks Wayland when available and X11 otherwise. Do not force `GDK_BACKEND=wayland`
on SteamOS: its desktop mode and Game mode (Gamescope) are X11 sessions, where GTK then
cannot start. Controls use each platform's own theme, light or dark (About > Theme).

## Build

wxWidgets is built from source. It is a git submodule in `vendor/wxWidgets`, pinned to
tag `v3.3.4` (commit `6a42a4b999b127cf28a8b5fe686da6b8ca93a9b8`), so clone with:

```
git clone --recursive https://github.com/tcottrill/skein-downloader.git
```

In a copy cloned without `--recursive`, fetch it with
`git submodule update --init --recursive`.

Windows (Visual Studio 2022 C++ tools and CMake):

```
powershell -ExecutionPolicy Bypass -File tools/build-windows.ps1
```

Or in the Visual Studio 2022 IDE: generate a solution with the `vs2022` preset and open
`build/vs2022/SkeinDownloader.sln` (Release builds go to `dist/`, Debug to `dist/debug/`).
Visual Studio's File > Open > Folder also picks up the preset.

```
cmake --preset vs2022
```

Windows installer (Inno Setup 6, `winget install JRSoftware.InnoSetup`), after the build:
`dist/SkeinDownloader-<version>-Setup.exe`. It installs for the current user without
administrator rights and needs an internet connection: it lists, then downloads and
checks, the video tools (yt-dlp, Deno, FFmpeg) from GitHub, and installs nothing if that
fails. Its uninstaller removes the program and the tools; settings and history in
`%LOCALAPPDATA%\SkeinDownloader` stay.

```
powershell -ExecutionPolicy Bypass -File tools/build-installer.ps1
```

Everything at once (Windows program and installer, then the AppImage and Flatpak through
WSL, then `dist/release/`; `-SkipLinux` for Windows only):

```
powershell -ExecutionPolicy Bypass -File tools/build-all.ps1
```

Linux (C++20 compiler, CMake, Ninja, pkg-config, GTK3, curl and OpenSSL development files).
On Ubuntu: `build-essential cmake ninja-build pkg-config libgtk-3-dev libcurl4-openssl-dev libssl-dev`.

```
bash tools/build-linux.sh
./dist/skein-downloader
```

An executable built inside the Freedesktop 24.08 SDK must run inside that SDK or runtime,
not against an older distribution's system libraries.

Flatpak (needs `flatpak` with the Freedesktop 24.08 SDK and Python 3), after the Linux build:

```
bash tools/package-flatpak.sh
```

On SteamOS install the Flatpak (`flatpak install --user ./SkeinDownloader-x86_64.flatpak`,
then `flatpak run io.github.tcottrill.SkeinDownloader//preview`) rather than copying
`dist/skein-downloader`.

AppImage, for Ubuntu 22.04 and newer without Flatpak. Any x86_64 Linux (or WSL) with bash,
curl and bubblewrap builds it, no root needed:

```
bash tools/build-linux-appimage.sh
```

The script builds inside a pinned Ubuntu 22.04 userland, runs the tests and a smoke test
there, and writes `dist/linux/SkeinDownloader-x86_64.AppImage` with yt-dlp, Deno and
FFmpeg bundled. It uses the system's GTK 3, which every Ubuntu desktop has, so the
desktop's theme and Wayland support apply. Run it with
`chmod +x SkeinDownloader-x86_64.AppImage && ./SkeinDownloader-x86_64.AppImage`; without
FUSE 2 (`libfuse2`) add `--appimage-extract-and-run`.

## Run

Windows: `dist/skein-downloader.exe`. Linux: `dist/skein-downloader`.
`--video` opens the video options. `--data-dir <absolute path>` uses a separate profile.
Only one instance may use a profile; a second launch brings the running window forward.

Settings and history live in:
- Windows: `%LOCALAPPDATA%/SkeinDownloader`
- Linux: `$XDG_DATA_HOME/SkeinDownloader`, or `~/.local/share/SkeinDownloader`
- Flatpak: `~/.var/app/io.github.tcottrill.SkeinDownloader/data/SkeinDownloader`

Each launch writes `skein-downloader.log` in that folder: startup environment, display
backend and theme, window layout, job state changes, download errors and retries, and
GTK/wx warnings. Include it when reporting a problem.

File downloads default to the normal Downloads folder; existing files are never overwritten.

## Video downloads

Video downloads use yt-dlp, FFmpeg/ffprobe and Deno.
- Windows: **Install / repair tools** in the video window runs `install-video-tools.ps1`
  (kept next to the executable), which downloads the official releases into
  `video-tools` and verifies their SHA-256 digests.
- Flatpak: yt-dlp and Deno are bundled; FFmpeg comes from the Freedesktop runtime.
- AppImage: yt-dlp, Deno and FFmpeg are bundled.
- Other Linux builds: set `SKEIN_VIDEO_TOOLS` to a folder containing Linux `yt-dlp`,
  `ffmpeg`, `ffprobe` and `deno`, or put them in `../libexec/skein-downloader` next to
  the executable.

Access to browser cookies is opt-in and is never restored across launches.

## Publishing a Linux release

The bundled yt-dlp executable and the AppImage's static FFmpeg contain GPL code, so each
Linux release must offer their source. `tools/package-flatpak.sh` and
`tools/build-linux-appimage.sh` write it to `dist/linux/sources/`: the yt-dlp and Deno
source releases, `video-tools-notices.txt` and, for the AppImage,
`ffmpeg-source-manifest.txt`, which lists every revision the bundled FFmpeg was built from.
Upload those files to the same release as the packages and keep them available as long as
they are offered.

## Known limits

Native themes differ between Windows and GTK. The indeterminate ("Working…") progress bar
is not animated. Wayland window activation is subject to the compositor's focus policy.
Mixed-DPI monitor changes on Windows and SteamOS/Batocera devices still need hands-on testing.

## Dependencies

Only wxWidgets core and base are linked; its image codecs, compression, regex, WebView,
media, XML, HTML/help, rich text, docking and editor modules are disabled. Windows
transfers use WinHTTP and Windows cryptography; Linux uses libcurl and OpenSSL. JSON
parsing uses the bundled nlohmann/json 3.12.0 header.

## License

Copyright (C) 2026 Tim Cottrill

This program is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.
It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See [LICENSE](LICENSE) for the full text.

Third-party components keep their own licenses: wxWidgets (wxWindows Library Licence,
`vendor/wxWidgets/docs/licence.txt`), nlohmann/json (MIT), and the yt-dlp, FFmpeg and
Deno helpers (see `packaging/licenses/`).
