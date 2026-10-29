#!/usr/bin/env bash
# fetch-tool-sources.sh CACHE OUT: saves the source releases of the bundled yt-dlp and
# Deno into OUT, to publish next to the Flatpak. Pinned to the versions
# tools/package-flatpak.sh ships; update them together.
set -euo pipefail
CACHE="$1" OUT="$2"
mkdir -p "$CACHE" "$OUT"

# source URL NAME SHA256: download into the cache once, verify, copy to OUT.
source_release() {
  local file="$CACHE/$2"
  if [ ! -f "$file" ] || ! echo "$3  $file" | sha256sum -c --status; then
    curl -fL --retry 3 -o "$file.part" "$1"
    mv "$file.part" "$file"
  fi
  echo "$3  $file" | sha256sum -c --quiet || { echo "Checksum mismatch: $2" >&2; exit 1; }
  cp "$file" "$OUT/$2"
}
source_release https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.tar.gz \
  yt-dlp-2026.08.19-source.tar.gz 072aad4f2a7604e92155f61a275a4752dc64046c8f6d90df3710525d94cd37c1
source_release https://github.com/denoland/deno/releases/download/v2.9.8/deno_src.tar.gz \
  deno-v2.9.8-source.tar.gz b57d0281c1647effc735f242ba52bc409747fc6ed6ad4946e76f062c67e113eb
