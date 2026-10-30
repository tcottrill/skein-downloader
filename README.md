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
cannot start.

## Downloads

Each release on the [Releases page](https://github.com/tcottrill/skein-downloader/releases)
has four downloads:

| Download | File |
|---|---|
| Windows installer (64-bit) | `SkeinDownloader-<version>-windows-x64-setup.exe` |
| Windows portable (64-bit) | `SkeinDownloader-<version>-windows-x64-portable.zip` |
| Linux Flatpak (SteamOS, any distribution with Flatpak) | `SkeinDownloader-<version>-linux-x86_64.flatpak` |
| Linux AppImage (Ubuntu 22.04 and newer) | `SkeinDownloader-<version>-linux-x86_64.AppImage` |

`SHA256SUMS.txt` lists their checksums. The release also carries the source of the
video tools bundled in the Linux packages (see [Publishing a release](#publishing-a-release)).

- **Windows installer:** installs for the current user without administrator rights,
  into `%LOCALAPPDATA%\Programs\Skein Downloader`, with a Start menu shortcut, an
  optional desktop shortcut and an entry in Settings > Apps. It needs an internet
  connection: a page lists the video tools it downloads (yt-dlp, Deno and FFmpeg from
  GitHub, about 255 MB, with their sources and licenses), and on Install it downloads
  them, showing each file, and checks each against the SHA-256 GitHub publishes. If
  that fails, nothing is installed. Silent installs (`/VERYSILENT`) download the same
  way. The uninstaller removes the program and the tools; settings and history stay.
  The installer is not code-signed yet, so Windows SmartScreen may warn before it runs.
- **Windows portable:** unzip anywhere and run `skein-downloader.exe`. Install the video
  tools from **Install / repair tools** in the video window on first use.
- **Flatpak:** `flatpak install --user ./SkeinDownloader-<version>-linux-x86_64.flatpak`,
  then `flatpak run io.github.tcottrill.SkeinDownloader//preview`. Use this on SteamOS.
- **AppImage:** `chmod +x SkeinDownloader-<version>-linux-x86_64.AppImage` and run it;
  without FUSE 2 (`libfuse2`) add `--appimage-extract-and-run`.

## Using it

- **+ File** and **+ Video** open their windows with the link on the clipboard when it
  is one they handle, sorted as the main window's **Load** sorts it: file hosts and file
  links go to File downloads, other video links to Video downloads. Otherwise the link
  box starts empty; the last link is not remembered. + File only fills in the link;
  click **Load files** to list the files.
- **Theme:** About > Theme chooses Same as system, Light or Dark. On Windows this is
  Windows' own dark mode (title bar, menus, buttons, lists, tabs, scroll bars) and
  applies the next time the program starts; on Linux it applies at once.

`--video` opens the video options. `--data-dir <absolute path>` uses a separate profile.
Only one instance may use a profile; a second launch brings the running window forward.

Settings (`settings.ini`) and history live in:
- Windows: `%LOCALAPPDATA%\SkeinDownloader`
- Linux: `$XDG_DATA_HOME/SkeinDownloader`, or `~/.local/share/SkeinDownloader`
- Flatpak: `~/.var/app/io.github.tcottrill.SkeinDownloader/data/SkeinDownloader`

Each launch writes `skein-downloader.log` in that folder: startup environment, display
backend and theme, window layout, job state changes, download errors and retries, and
GTK/wx warnings. Include it when reporting a problem.

File downloads default to the normal Downloads folder; existing files are never overwritten.

## Video downloads

Video downloads use yt-dlp, FFmpeg/ffprobe and Deno.
- Windows installer: Setup downloads them into `video-tools` next to the program.
- Windows (installer or portable): **Install / repair tools** in the video window runs
  `install-video-tools.ps1` (kept next to the executable), which downloads the official
  releases into `video-tools` and verifies their SHA-256 digests. The same window
  updates yt-dlp.
- Flatpak: yt-dlp and Deno are bundled; FFmpeg comes from the Freedesktop runtime.
- AppImage: yt-dlp, Deno and FFmpeg are bundled.
- Other Linux builds: set `SKEIN_VIDEO_TOOLS` to a folder containing Linux `yt-dlp`,
  `ffmpeg`, `ffprobe` and `deno`, or put them in `../libexec/skein-downloader` next to
  the executable.

Access to browser cookies is opt-in and is never restored across launches.

## Build

wxWidgets is built from source. It is a git submodule in `vendor/wxWidgets`, pinned to
tag `v3.3.4` (commit `6a42a4b999b127cf28a8b5fe686da6b8ca93a9b8`), so clone with:

```
git clone --recursive https://github.com/tcottrill/skein-downloader.git
```

In a copy cloned without `--recursive`, fetch it with
`git submodule update --init --recursive`.

Build output goes to two folders:
- `dist/`: the program itself, for running and testing (`dist/skein-downloader.exe` on
  Windows, `dist/skein-downloader` on Linux; Visual Studio Debug builds in `dist/debug/`).
- `dist/release/`: the four downloads, `SHA256SUMS.txt` and the tool sources, named with
  the version from `CMakeLists.txt`. Upload everything in it to the GitHub release.

Intermediate files go to `build/`, which can be deleted at any time; the next build
recreates it (and takes longer).

### Everything at once

On Windows, with the tools below installed:

```
powershell -ExecutionPolicy Bypass -File tools/build-all.ps1
```

It builds the Windows program and installer, then the AppImage and Flatpak through WSL,
then collects `dist/release/`. `-SkipLinux` builds only Windows; `-Distro <name>` picks
the WSL distribution (default `Ubuntu`).

Needed:
- Visual Studio 2022 with the C++ desktop tools (CMake and Ninja come with it).
- Inno Setup 6 for the installer: `winget install JRSoftware.InnoSetup`.
- For Linux: a WSL distribution with bash, curl, bubblewrap and `flatpak` with the
  `org.freedesktop.Sdk//24.08` SDK, and Python 3.

### Windows step by step

```
powershell -ExecutionPolicy Bypass -File tools/build-windows.ps1
powershell -ExecutionPolicy Bypass -File tools/build-installer.ps1
powershell -ExecutionPolicy Bypass -File tools/collect-release.ps1
```

`build-windows.ps1` writes `dist/skein-downloader.exe`. `build-installer.ps1` writes the
installer to `build/installer/` from `packaging/windows/skein-downloader.iss`.
`collect-release.ps1` gathers whatever has been built into `dist/release/` and reports
anything missing.

In the Visual Studio 2022 IDE: generate a solution with the `vs2022` preset and open
`build/vs2022/SkeinDownloader.sln` (Release builds go to `dist/`, Debug to `dist/debug/`;
F5 starts the program). Visual Studio's File > Open > Folder also picks up the preset.

```
cmake --preset vs2022
```

### Linux

Linux (C++20 compiler, CMake, Ninja, pkg-config, GTK3, curl and OpenSSL development files).
On Ubuntu: `build-essential cmake ninja-build pkg-config libgtk-3-dev libcurl4-openssl-dev libssl-dev`.

```
bash tools/build-linux.sh
./dist/skein-downloader
```

An executable built inside the Freedesktop 24.08 SDK must run inside that SDK or runtime,
not against an older distribution's system libraries.

Flatpak (needs `flatpak` with the Freedesktop 24.08 SDK and Python 3), after the Linux
build inside that SDK:

```
flatpak run --filesystem=$PWD --share=network --command=bash org.freedesktop.Sdk//24.08 -c "bash tools/build-linux.sh"
bash tools/package-flatpak.sh
```

It writes `dist/linux/SkeinDownloader-x86_64.flatpak`. On SteamOS install the Flatpak
rather than copying `dist/skein-downloader`.

AppImage, for Ubuntu 22.04 and newer without Flatpak. Any x86_64 Linux (or WSL) with bash,
curl and bubblewrap builds it, no root needed:

```
bash tools/build-linux-appimage.sh
```

The script builds inside a pinned Ubuntu 22.04 userland, runs a smoke test there, and
writes `dist/linux/SkeinDownloader-x86_64.AppImage` with yt-dlp, Deno and FFmpeg bundled.
It uses the system's GTK 3, which every Ubuntu desktop has, so the desktop's theme and
Wayland support apply. `collect-release.ps1` adds the version and `linux-` to both
package names when it copies them to `dist/release/`.

### Icons

`icons/` holds the master artwork (1024 px, Windows .ico and Linux sizes).
`tools/make-icons.py` (Python 3 with Pillow) builds `packaging/icons/` from it: the
Windows `.ico` and the Linux icon theme sizes. Run it again whenever the artwork changes,
then rebuild.

## Publishing a release

The bundled yt-dlp executable and the AppImage's static FFmpeg contain GPL code, so each
Linux release must offer their source. `tools/package-flatpak.sh` and
`tools/build-linux-appimage.sh` write it to `dist/linux/sources/`, and
`collect-release.ps1` copies it to `dist/release/`: the yt-dlp and Deno source releases,
`video-tools-notices.txt` and, for the AppImage, `ffmpeg-source-manifest.txt`, which lists
every revision the bundled FFmpeg was built from. Upload those files to the same release
as the packages and keep them available as long as they are offered.

## Known limits

Native themes differ between Windows and GTK. On Windows a theme change applies after a
restart. The indeterminate ("Working…") progress bar is not animated. Wayland window
activation is subject to the compositor's focus policy. Mixed-DPI monitor changes on
Windows and SteamOS/Batocera devices still need hands-on testing. The Windows installer
is not code-signed.

## Dependencies

Only wxWidgets core and base are linked, with PNG and zlib for the toolbar's Ko-fi logo;
its other image formats (JPEG, TIFF, WebP, SVG), regex, WebView, media, XML, HTML/help,
rich text, docking and editor modules are disabled. Windows transfers use WinHTTP and
Windows cryptography; Linux uses libcurl and OpenSSL. JSON parsing uses the bundled
nlohmann/json 3.12.0 header.

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
