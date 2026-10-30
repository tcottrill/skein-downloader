#!/usr/bin/env bash
# fetch-tool-sources.sh CACHE OUT: saves the source release of the bundled yt-dlp into
# OUT, to publish next to the Linux packages (its standalone executable contains GPL
# code). Deno is MIT, which asks only for its notice, so its source is not shipped.
# Pinned to the version tools/package-flatpak.sh ships; update them together.
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