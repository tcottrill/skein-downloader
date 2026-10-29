#!/usr/bin/env python3
"""Writes the corresponding-source manifest for the bundled static FFmpeg build.

usage: ffmpeg-source-manifest.py FFMPEG_BUILDS_DIR FFMPEG_COMMIT BUILDS_COMMIT BINARY_URL BINARY_SHA256

FFMPEG_BUILDS_DIR is yt-dlp/FFmpeg-Builds at BUILDS_COMMIT. Each stage script's pinned
repository and revision is read as text, and its ffbuild_enabled conditions are evaluated
for target linux64 and variant gpl, the build the AppImage ships. The third-party scripts
are never executed; a condition this parser does not recognise stops it with an error,
so a changed recipe is noticed rather than silently mislisted.
"""
import fnmatch
import pathlib
import re
import sys

ENV = {'TARGET': 'linux64', 'VARIANT': 'gpl', 'ADDINS_STR': ''}
FFVER = 99999999  # util/vars.sh value for a master build without version addins
TOKEN = re.compile(r'\[\[(.*?)\]\]|\(\((.*?)\)\)|\(\s*source\s+(\S+)\s*&&\s*ffbuild_enabled\s*\)'
                   r'|return\s+(-?\d+)|(&&)|(\|\|)')


def test(text):
    """One [[ ]] expression: '$VAR == glob' / '$VAR != glob' joined by &&."""
    result = True
    for part in text.split('&&'):
        m = re.fullmatch(r'\s*\$(\w+)\s*(==|!=)\s*(\S+)\s*', part)
        if not m or m[1] not in ENV:
            raise ValueError(f'unrecognised test: [[{text}]]')
        hit = fnmatch.fnmatchcase(ENV[m[1]], m[3])
        result = result and (hit if m[2] == '==' else not hit)
    return result


def arith(text):
    m = re.fullmatch(r'\s*\$\(ffbuild_ffver\)\s*(>=|<=|>|<|==)\s*(\d+)\s*', text)
    if not m:
        raise ValueError(f'unrecognised arithmetic: (({text}))')
    n = int(m[2])
    return {'>=': FFVER >= n, '<=': FFVER <= n, '>': FFVER > n, '<': FFVER < n, '==': FFVER == n}[m[1]]


def enabled(root, script):
    body = re.search(r'^ffbuild_enabled\(\) \{\n(.*?)^\}', (root / script).read_text(), re.S | re.M)
    if not body:
        raise ValueError(f'{script}: no ffbuild_enabled')
    status = True
    for line in body[1].splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        pos, run, op = 0, True, None
        for m in TOKEN.finditer(line):
            if line[pos:m.start()].strip():
                raise ValueError(f'{script}: unrecognised statement: {line}')
            pos = m.end()
            if m[5] or m[6]:
                op = m[5] or m[6]
                continue
            # Bash list semantics: '&&' runs the next command after success, '||' after failure.
            run = op is None or (op == '&&') == status
            if not run:
                continue
            if m[4] is not None:
                return int(m[4]) == 0
            status = test(m[1]) if m[1] is not None else arith(m[2]) if m[2] is not None else enabled(root, m[3])
        if line[pos:].strip():
            raise ValueError(f'{script}: unrecognised statement: {line}')
    return status


def pins(path):
    return dict(re.findall(r'^(SCRIPT_[A-Z0-9_]+)="([^"]*)"$', path.read_text(), re.M))


def ct(root, key):
    m = re.search(rf'^CT_{key}="(.*)"$', (root / 'images/base-linux64/ct-ng-config').read_text(), re.M)
    return m[1] if m else '?'


def main(builds, ffmpeg_commit, builds_commit, binary_url, binary_sha256):
    root = pathlib.Path(builds)
    v = {k: ct(root, k) for k in ('VERSION', 'GLIBC_VERSION', 'GCC_VERSION', 'BINUTILS_VERSION', 'LINUX_VERSION')}
    out = [f'''Corresponding source for the FFmpeg and ffprobe programs in Skein Downloader's AppImage
========================================================================================

The AppImage ships ffmpeg and ffprobe unmodified from this official yt-dlp/FFmpeg-Builds
release (GPL-3.0-or-later; see ffmpeg-LICENSE.txt):
  {binary_url}
  sha256 {binary_sha256}

That binary is built from the sources below. Every revision is exact, so the same
source can be fetched from each project's public repository. The build recipe also
holds the patches, configure options and toolchain configuration that were applied.

Build recipe (target linux64, variant gpl)
  https://github.com/yt-dlp/FFmpeg-Builds  commit {builds_commit}
  Archive: https://github.com/yt-dlp/FFmpeg-Builds/archive/{builds_commit}.tar.gz

FFmpeg
  https://github.com/FFmpeg/FFmpeg  commit {ffmpeg_commit}
  Archive: https://github.com/FFmpeg/FFmpeg/archive/{ffmpeg_commit}.tar.gz
  Patches applied: patches/master/ in the build recipe.

Toolchain (crosstool-ng {v['VERSION']}, images/base-linux64/ct-ng-config in the recipe).
glibc is linked statically (LGPL-2.1-or-later); the GCC runtime libraries carry the
GCC Runtime Library Exception.
  glibc {v['GLIBC_VERSION']}  https://ftp.gnu.org/gnu/glibc/glibc-{v['GLIBC_VERSION']}.tar.xz
  gcc {v['GCC_VERSION']}  https://ftp.gnu.org/gnu/gcc/gcc-{v['GCC_VERSION']}/gcc-{v['GCC_VERSION']}.tar.xz
  binutils {v['BINUTILS_VERSION']}  https://ftp.gnu.org/gnu/binutils/binutils-{v['BINUTILS_VERSION']}.tar.xz
  Linux headers {v['LINUX_VERSION']}  https://cdn.kernel.org/pub/linux/kernel/v{v['LINUX_VERSION'].split('.')[0]}.x/linux-{v['LINUX_VERSION']}.tar.xz

Libraries built into ffmpeg/ffprobe (stage, repository, revision). Each revision
includes the git submodules it pins; the Rust crates of rav1e and librsvg are the ones
locked by their Cargo.lock at that revision. The stage scripts of the same names in
scripts.d/ of the build recipe show how each one was fetched and built.
''']
    stages = sorted(root.glob('scripts.d/*.sh')) + sorted(root.glob('scripts.d/*/*.sh'))
    for path in stages:
        script = path.relative_to(root).as_posix()
        if not enabled(root, script):
            continue
        p = pins(path)
        for n in ('', '2', '3'):
            url = p.get(f'SCRIPT_REPO{n}') or p.get(f'SCRIPT_MIRROR{n}')
            if not url:
                continue
            rev = p.get(f'SCRIPT_COMMIT{n}') or (f"r{p[f'SCRIPT_REV{n}']}" if f'SCRIPT_REV{n}' in p else None)
            if not rev:
                raise ValueError(f'{script}: {url} has no pinned revision')
            out.append(f'  {path.stem:<28} {url}  {rev}\n')
    sys.stdout.write(''.join(out))


if __name__ == '__main__':
    if len(sys.argv) != 6:
        sys.exit(__doc__.split('\n\n')[1])
    main(*sys.argv[1:])
