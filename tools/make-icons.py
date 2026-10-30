#!/usr/bin/env python3
"""Builds packaging/icons from the artwork in icons/ (the designer's export).

usage: make-icons.py [SOURCE_DIR] [OUT_DIR]     defaults: icons packaging/icons

- hicolor/<n>x<n>/apps/io.github.tcottrill.SkeinDownloader.png for the Linux sizes the
  icon theme defines (16-512; the 1024 master is not a theme size).
- skein-downloader.ico for Windows, from the same images as the designer's .ico: classic
  32-bit bitmaps up to 128 px and PNG only for 256 px, the one size every Windows icon
  loader accepts compressed.
Needs Pillow. Run it again whenever the artwork changes, then rebuild.
"""
import io
import pathlib
import shutil
import struct
import sys

from PIL import Image

APP_ID = 'io.github.tcottrill.SkeinDownloader'
LINUX_SIZES = (16, 22, 24, 32, 48, 64, 96, 128, 192, 256, 512)


def ico_images(path):
    """(size, RGBA image) for each entry of an .ico, PNG- or bitmap-encoded."""
    data = path.read_bytes()
    count = struct.unpack_from('<HHH', data)[2]
    images = []
    for i in range(count):
        w, h, _, _, _, _, length, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        blob = data[offset:offset + length]
        if blob[:4] == b'\x89PNG':
            image = Image.open(io.BytesIO(blob)).convert('RGBA')
        else:
            single = struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', w, h, 0, 0, 1, 32, length, 22) + blob
            image = Image.open(io.BytesIO(single)).convert('RGBA')
        images.append((image.width, image))
    return sorted(images, key=lambda item: item[0])


def dib(image):
    """A 32-bit BGRA bitmap entry with its AND mask, as icon resources store them."""
    w, h = image.size
    header = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = image.getpixel((x, y))
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    mask_stride = ((w + 31) // 32) * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        bits = bytearray(mask_stride)
        for x in range(w):
            if image.getpixel((x, y))[3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + b''.join(rows) + bytes(mask)


def write_ico(images, path):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for size, image in images:
        if size >= 256:
            buffer = io.BytesIO(); image.save(buffer, 'PNG'); blob = buffer.getvalue()
        else:
            blob = dib(image)
        entries.append(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    path.write_bytes(struct.pack('<HHH', 0, 1, len(images)) + b''.join(entries) + b''.join(blobs))


def main(source='icons', out='packaging/icons'):
    source, out = pathlib.Path(source), pathlib.Path(out)
    for size in LINUX_SIZES:
        target = out / 'hicolor' / f'{size}x{size}' / 'apps' / f'{APP_ID}.png'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / 'linux' / 'hicolor' / f'{size}x{size}' / 'apps' / 'downloader.png', target)
        if Image.open(target).size != (size, size):
            raise SystemExit(f'{target}: not {size}x{size}')
    images = ico_images(source / 'windows' / 'downloader.ico')
    write_ico(images, out / 'skein-downloader.ico')
    print('Linux sizes:', ', '.join(map(str, LINUX_SIZES)))
    print('Windows .ico sizes:', ', '.join(str(size) for size, _ in images))


if __name__ == '__main__':
    main(*sys.argv[1:])
