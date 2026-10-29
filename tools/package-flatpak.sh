#!/usr/bin/env bash
# Packages dist/skein-downloader (built by tools/build-linux.sh) as
# dist/linux/SkeinDownloader-x86_64.flatpak, with pinned yt-dlp and Deno releases.
# FFmpeg comes from the Freedesktop runtime. The source releases of the bundled tools
# go to dist/linux/sources/, to publish next to the bundle.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ID=io.github.tcottrill.SkeinDownloader
WORK="${SKEIN_FLATPAK_WORK:-$HOME/skein-downloader-flatpak}"
CACHE="$WORK/cache"
OUT="$ROOT/dist/linux"
mkdir -p "$CACHE" "$OUT"

# fetch URL FILE SHA256: download into the cache once and always verify.
fetch() {
  local file="$CACHE/$2"
  if [ ! -f "$file" ] || ! echo "$3  $file" | sha256sum -c --status; then
    curl -fL --retry 3 -o "$file.part" "$1"
    mv "$file.part" "$file"
  fi
  echo "$3  $file" | sha256sum -c --quiet || { echo "Checksum mismatch: $2" >&2; exit 1; }
}
# Keep these versions in step with tools/fetch-tool-sources.sh and video-tools-notices.txt.
fetch https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp_linux \
  yt-dlp_linux 58162f9bfdc27458ea47bfcb311cf47028f17d8154a8bf7d689861d46399230a
fetch https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-unknown-linux-gnu.zip \
  deno.zip e890975bc2549807bbcc0b584ffafda72cc7c93349dd99c80483dd784f6c4609
bash "$ROOT/tools/fetch-tool-sources.sh" "$CACHE" "$OUT/sources"

STAGE="$(mktemp -d "$WORK/stage-XXXXXX")"
flatpak build-init "$STAGE" "$ID" org.freedesktop.Sdk org.freedesktop.Platform 24.08
install -Dm755 "$ROOT/dist/skein-downloader" "$STAGE/files/bin/skein-downloader"
install -m755 "$CACHE/yt-dlp_linux" "$STAGE/files/bin/yt-dlp"
python3 -m zipfile -e "$CACHE/deno.zip" "$STAGE/deno" && install -m755 "$STAGE/deno/deno" "$STAGE/files/bin/deno" && rm -rf "$STAGE/deno"
ln -s /usr/bin/ffmpeg "$STAGE/files/bin/ffmpeg"
ln -s /usr/bin/ffprobe "$STAGE/files/bin/ffprobe"
install -Dm644 "$ROOT/packaging/$ID.desktop" "$STAGE/files/share/applications/$ID.desktop"
install -Dm644 "$ROOT/packaging/$ID.metainfo.xml" "$STAGE/files/share/metainfo/$ID.metainfo.xml"
install -Dm644 "$ROOT/packaging/$ID.svg" "$STAGE/files/share/icons/hicolor/scalable/apps/$ID.svg"
LICENSES="$STAGE/files/share/licenses/skein-downloader"
mkdir -p "$LICENSES"
install -m644 "$ROOT/LICENSE" "$ROOT/packaging/licenses/"* "$LICENSES/"
install -m644 "$ROOT/vendor/wxWidgets/docs/licence.txt" "$LICENSES/wxWidgets.txt"
install -m644 "$ROOT/packaging/licenses/video-tools-notices.txt" "$OUT/sources/"
flatpak build-finish --command=skein-downloader --share=network --share=ipc --socket=wayland --socket=fallback-x11 --device=dri --filesystem=xdg-download "$STAGE"
flatpak build-export "$WORK/repo" "$STAGE" preview
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo "$WORK/repo" "$OUT/SkeinDownloader-x86_64.flatpak" "$ID" preview
(cd "$OUT" && sha256sum SkeinDownloader-x86_64.flatpak > SkeinDownloader-x86_64.flatpak.sha256)
printf '%s\n' "$STAGE" > "$WORK/latest-stage.txt"
echo "Flatpak: $OUT/SkeinDownloader-x86_64.flatpak"
echo "Publish everything in $OUT/sources/ with it."
