# Building Skein Downloader

[Back to the README](README.md)

Windows uses wxMSW with wxWidgets and the C runtime linked statically. Linux uses wxGTK3,
which picks Wayland when available and X11 otherwise. Do not force `GDK_BACKEND=wayland`
on SteamOS: its desktop mode and Game mode (Gamescope) are X11 sessions, where GTK then
cannot start.

## Getting the source

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

## Dependencies

Only wxWidgets core and base are linked, with PNG and zlib for the toolbar's Ko-fi logo;
its other image formats (JPEG, TIFF, WebP, SVG), regex, WebView, media, XML, HTML/help,
rich text, docking and editor modules are disabled. Windows transfers use WinHTTP and
Windows cryptography; Linux uses libcurl and OpenSSL. JSON parsing uses the bundled
nlohmann/json 3.12.0 header.


## Website preview and future hosting

The static showcase is `index.html` at the repository root. Open it directly in a browser, or serve the repository with `python -m http.server 8000` and visit `http://localhost:8000`. It uses the existing `images/` directory and requires no JavaScript dependencies or build step.

Nothing is published automatically. When ready to publish, push the site and assets, enable GitHub Pages for the repository, and select the desired branch and `/ (root)` as the source. Confirm the release links and package availability before announcing it. The page links to the release listing rather than guessing versioned asset URLs.
