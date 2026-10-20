#!/usr/bin/env python3
"""Build a macOS .icns from the icon embedded in the game's own executable.

    tools/recomp/macos_icon.py --exe "Petz 5.exe" --build-root build

The .icns is cached under <build root>/macos-icon and regenerated only when the
executable's SHA-256 changes, so a runtime-only build does not unpack the icon
again. Nothing here is committed: the icon is the user's game art, read from the
user's installation.

The PE resource reader and the DIB decoder are local to this file so the step
has no required third-party dependency. It understands the ICO frames a Windows
executable carries: 32-bit BGRA with an AND mask, 24-bit BGR, 8-, 4- and 1-bit
palette images, and PNG-compressed frames when Pillow is installed. Pillow also
encodes the PNGs when available; otherwise a small RGBA PNG writer here does it.
Scaling is nearest-neighbour, because every macOS iconset size is an integer
multiple of the 16x16 and 32x32 frames these old icons carry, and smoothing a
pixel-art icon would invent detail the game never had.
"""

import argparse
import hashlib
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

try:
    from PIL import Image
except ImportError:  # PNG encoding falls back to the writer below.
    Image = None

# Bump when the extraction or rendering changes, to invalidate the cache.
CACHE_VERSION = 3
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# The names iconutil expects, with the pixel size each file must contain.
ICONSET = (
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
)
RT_ICON = 3
RT_GROUP_ICON = 14


class IconError(ValueError):
    """The executable has no icon this tool can read."""


def _u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _i32(data, offset):
    return struct.unpack_from("<i", data, offset)[0]


class PEImage:
    """Enough of a PE/COFF image to walk its resource directory."""

    def __init__(self, data):
        self.data = data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise IconError("not a Windows executable")
        pe = _u32(data, 0x3C)
        if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
            raise IconError("no PE header")
        coff = pe + 4
        sections = _u16(data, coff + 2)
        optional_size = _u16(data, coff + 16)
        optional = coff + 20
        magic = _u16(data, optional)
        if magic == 0x10B:
            directories = optional + 96
        elif magic == 0x20B:
            directories = optional + 112
        else:
            raise IconError("unknown optional header")
        self.size_of_headers = _u32(data, optional + 60)
        self.resource_rva = _u32(data, directories + 2 * 8)
        self.resource_size = _u32(data, directories + 2 * 8 + 4)
        if not self.resource_rva or not self.resource_size:
            raise IconError("the executable has no resource directory")
        self.sections = []
        table = optional + optional_size
        for index in range(sections):
            base = table + index * 40
            if base + 40 > len(data):
                break
            virtual_size = _u32(data, base + 8)
            virtual_address = _u32(data, base + 12)
            raw_size = _u32(data, base + 16)
            raw_pointer = _u32(data, base + 20)
            self.sections.append((virtual_address, max(virtual_size, raw_size), raw_size, raw_pointer))
        self.resource_offset = self.rva_to_offset(self.resource_rva)

    def rva_to_offset(self, rva):
        """A file offset for an RVA, or raise when it lies outside the image."""
        if rva < self.size_of_headers and rva < len(self.data):
            return rva
        for virtual_address, span, raw_size, raw_pointer in self.sections:
            if virtual_address <= rva < virtual_address + span:
                delta = rva - virtual_address
                if delta < raw_size:
                    return raw_pointer + delta
                break
        raise IconError("RVA %#x is not backed by file data" % rva)

    def _directory(self, relative):
        offset = self.resource_offset + relative
        if offset + 16 > len(self.data):
            raise IconError("truncated resource directory")
        named = _u16(self.data, offset + 12)
        ids = _u16(self.data, offset + 14)
        entries = []
        for index in range(named + ids):
            entry = offset + 16 + index * 8
            if entry + 8 > len(self.data):
                break
            name = _u32(self.data, entry)
            target = _u32(self.data, entry + 4)
            entries.append((name & 0x7FFFFFFF, bool(name & 0x80000000),
                            bool(target & 0x80000000), target & 0x7FFFFFFF))
        return entries

    def _first_data(self, relative, depth=0):
        """The first IMAGE_RESOURCE_DATA_ENTRY reachable below a directory."""
        if depth > 4:
            return None
        for identifier, named, is_directory, target in self._directory(relative):
            if named:
                continue
            if is_directory:
                data = self._first_data(target, depth + 1)
                if data is not None:
                    return data
                continue
            entry = self.resource_offset + target
            if entry + 16 > len(self.data):
                return None
            rva = _u32(self.data, entry)
            size = _u32(self.data, entry + 4)
            offset = self.rva_to_offset(rva)
            return self.data[offset:offset + size]
        return None

    def icons(self):
        """RT_GROUP_ICON blobs by id and RT_ICON blobs by id."""
        groups, images = {}, {}
        for identifier, named, is_directory, target in self._directory(0):
            if named or not is_directory:
                continue
            if identifier == RT_GROUP_ICON:
                for icon_id, _, _, icon_target in self._directory(target):
                    blob = self._first_data(icon_target)
                    if blob:
                        groups[icon_id] = blob
            elif identifier == RT_ICON:
                for icon_id, _, _, icon_target in self._directory(target):
                    blob = self._first_data(icon_target)
                    if blob:
                        images[icon_id] = blob
        return groups, images


def _mask_bit(blob, base, stride, x, y):
    offset = base + y * stride + (x >> 3)
    if offset >= len(blob):
        return 0
    return (blob[offset] >> (7 - (x & 7))) & 1


def _channel(value, mask):
    """One colour channel from a BI_BITFIELDS word."""
    if not mask:
        return 0
    shift = (mask & -mask).bit_length() - 1
    bits = bin(mask).count("1")
    raw = (value & mask) >> shift
    return raw * 255 // ((1 << bits) - 1)


def decode_dib(blob):
    """A Windows DIB icon frame as (width, height, RGBA bytes)."""
    if len(blob) < 40:
        raise IconError("truncated DIB header")
    header = _u32(blob, 0)
    if header < 40 or header > len(blob):
        raise IconError("unsupported DIB header")
    width = _i32(blob, 4)
    stored_height = _i32(blob, 8)
    planes = _u16(blob, 12)
    bpp = _u16(blob, 14)
    compression = _u32(blob, 16)
    colors_used = _u32(blob, 32)
    if width <= 0 or stored_height == 0 or planes != 1:
        raise IconError("invalid DIB geometry")
    if compression not in (0, 3) or (compression == 3 and bpp != 32):
        raise IconError("unsupported DIB compression %d" % compression)
    if bpp not in (1, 4, 8, 24, 32):
        raise IconError("unsupported DIB depth %d" % bpp)
    # ICO stores the XOR bitmap and then the AND mask, so the header height is
    # twice the image height. A non-conforming file may store the real height.
    height = abs(stored_height) // 2 if abs(stored_height) % 2 == 0 else abs(stored_height)
    if height == 0:
        height = abs(stored_height)

    palette = []
    if bpp <= 8:
        count = colors_used or (1 << bpp)
        offset = header
        palette_size = count * 4
        if offset + palette_size > len(blob):
            raise IconError("truncated DIB palette")
        for index in range(count):
            blue, green, red, _alpha = blob[offset + index * 4:offset + index * 4 + 4]
            palette.append((red, green, blue))

    pixel_offset = header
    channel_masks = None
    if compression == 3:
        if header + 12 > len(blob):
            raise IconError("truncated DIB bit masks")
        channel_masks = struct.unpack_from("<III", blob, header)
        pixel_offset = header + 12
    pixel_offset += len(palette) * 4
    row_stride = ((width * bpp + 31) // 32) * 4
    mask_stride = ((width + 31) // 32) * 4
    mask_offset = pixel_offset + row_stride * height
    if pixel_offset + row_stride * height > len(blob):
        raise IconError("truncated DIB pixels")

    top_down = stored_height < 0
    rows = []
    alpha_present = False
    for row in range(height):
        y = row if top_down else height - 1 - row
        source = pixel_offset + y * row_stride
        pixels = []
        for x in range(width):
            if bpp == 32:
                offset = source + x * 4
                blue, green, red, alpha = blob[offset:offset + 4]
                if channel_masks:
                    word = _u32(blob, offset)
                    red = _channel(word, channel_masks[0])
                    green = _channel(word, channel_masks[1])
                    blue = _channel(word, channel_masks[2])
                    alpha = _channel(word, channel_masks[3]) if channel_masks[3] else 255
                if alpha:
                    alpha_present = True
            elif bpp == 24:
                offset = source + x * 3
                blue, green, red = blob[offset:offset + 3]
                alpha = 255
            else:
                if bpp == 8:
                    index = blob[source + x]
                elif bpp == 4:
                    byte = blob[source + (x >> 1)]
                    index = (byte >> 4) if x % 2 == 0 else (byte & 0x0F)
                else:
                    byte = blob[source + (x >> 3)]
                    index = (byte >> (7 - (x & 7))) & 1
                if index >= len(palette):
                    red = green = blue = 0
                else:
                    red, green, blue = palette[index]
                alpha = 255
            pixels.append((red, green, blue, alpha))
        rows.append(pixels)

    # Old icons often carry a 32-bit bitmap whose alpha bytes are all zero; the
    # AND mask is then the only transparency information. When some alpha is
    # present it wins, but a set mask bit still clips the pixel.
    use_mask = not alpha_present or bpp != 32
    rgba = bytearray(width * height * 4)
    for source_row, y in enumerate(rows):
        # The AND mask is stored in the same bottom-up order as the XOR
        # bitmap; `rows` is already normalised to top-down.
        mask_row = source_row if top_down else height - 1 - source_row
        for x, (red, green, blue, alpha) in enumerate(y):
            if bpp != 32:
                alpha = 255
            elif not alpha_present:
                alpha = 255
            if use_mask and _mask_bit(blob, mask_offset, mask_stride, x, mask_row):
                alpha = 0
            offset = (source_row * width + x) * 4
            rgba[offset:offset + 4] = bytes((red, green, blue, alpha))
    return width, height, bytes(rgba)


def decode_frame(blob):
    """One icon frame, PNG-compressed or DIB, as (width, height, RGBA bytes)."""
    if blob.startswith(PNG_SIGNATURE):
        if Image is None:
            raise IconError("PNG-compressed icon frame needs Pillow")
        with Image.open(io.BytesIO(blob)) as image:
            image = image.convert("RGBA")
            return image.width, image.height, image.tobytes()
    return decode_dib(blob)


def frames_from_exe(exe_path):
    """Every readable frame of the executable's highest-quality icon group.

    Groups are ranked by their largest frame, so a small secondary group (a
    document icon, say) never replaces the application icon."""
    image = PEImage(Path(exe_path).read_bytes())
    groups, images = image.icons()
    if not groups or not images:
        raise IconError("%s has no icon resources" % exe_path)
    best = None
    for group in groups.values():
        if len(group) < 6:
            continue
        count = _u16(group, 4)
        entries = []
        for index in range(count):
            entry = 6 + index * 14
            if entry + 14 > len(group):
                break
            width, height, _colors, _reserved, _planes, bpp, _size, icon_id = \
                struct.unpack_from("<BBBBHHIH", group, entry)
            entries.append((width or 256, height or 256, bpp, icon_id))
        if not entries:
            continue
        score = max(width * height for width, height, _bpp, _id in entries)
        if best is None or score > best[0]:
            best = (score, entries)
    if best is None:
        raise IconError("%s has no usable icon group" % exe_path)
    frames = {}
    for _width, _height, bpp, icon_id in best[1]:
        blob = images.get(icon_id)
        if blob is None:
            continue
        try:
            width, height, rgba = decode_frame(blob)
        except (IconError, OSError, ValueError):
            continue
        if width <= 0 or height <= 0 or len(rgba) != width * height * 4:
            continue
        # One frame per size; the deepest one is the highest quality.
        previous = frames.get((width, height))
        if previous is None or bpp > previous[0]:
            frames[(width, height)] = (bpp, rgba)
    if not frames:
        raise IconError("%s has no decodable icon frame" % exe_path)
    return [(width, height, value[1]) for (width, height), value in sorted(frames.items())]


def _pick(frames, size):
    """The frame to render `size` from: exact, else the smallest larger one."""
    exact = [frame for frame in frames if frame[0] == size and frame[1] == size]
    if exact:
        return exact[0]
    larger = [frame for frame in frames if frame[0] >= size and frame[1] >= size]
    if larger:
        return min(larger, key=lambda frame: frame[0] * frame[1])
    return max(frames, key=lambda frame: frame[0] * frame[1])


def _nearest(width, height, rgba, size):
    """Scale to a square by repeating pixels. Every iconset size is an integer
    multiple of a 16 or 32 pixel frame, so no interpolation is needed."""
    if width == size and height == size:
        return rgba
    if Image is not None:
        image = Image.frombytes("RGBA", (width, height), rgba)
        return image.resize((size, size), Image.NEAREST).tobytes()
    output = bytearray(size * size * 4)
    for y in range(size):
        source_y = y * height // size
        source_row = source_y * width * 4
        target_row = y * size * 4
        for x in range(size):
            source_x = x * width // size
            start = source_row + source_x * 4
            output[target_row + x * 4:target_row + x * 4 + 4] = rgba[start:start + 4]
    return bytes(output)


def _png_chunk(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def _write_png(path, size, rgba):
    """A dependency-free RGBA PNG writer, used when Pillow is absent."""
    raw = bytearray()
    row_bytes = size * 4
    for y in range(size):
        raw.append(0)  # filter: none
        raw += rgba[y * row_bytes:(y + 1) * row_bytes]
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    path.write_bytes(PNG_SIGNATURE + _png_chunk(b"IHDR", header)
                     + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
                     + _png_chunk(b"IEND", b""))


def write_png(path, size, rgba):
    if Image is not None:
        Image.frombytes("RGBA", (size, size), rgba).save(path, "PNG")
    else:
        _write_png(path, size, rgba)


def write_iconset(frames, directory):
    """Write the ten files iconutil reads; returns the directory."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, size in ICONSET:
        width, height, rgba = _pick(frames, size)
        write_png(directory / name, size, _nearest(width, height, rgba, size))
    return directory


def build_icns(exe_path, output, work_dir=None):
    """Extract the executable's icon and write an .icns at `output`."""
    output = Path(output)
    frames = frames_from_exe(exe_path)
    temporary = Path(work_dir or output.parent)
    temporary.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="AppIcon-", dir=temporary))
    try:
        iconset = write_iconset(frames, staging / "AppIcon.iconset")
        output.parent.mkdir(parents=True, exist_ok=True)
        # iconutil insists on an .icns suffix, so the partial output cannot be
        # "AppIcon.icns.partial".
        partial = output.with_name(output.stem + ".partial.icns")
        completed = subprocess.run(["iconutil", "-c", "icns", "-o", str(partial), str(iconset)],
                                   capture_output=True, text=True)
        if completed.returncode:
            raise IconError("iconutil failed: %s" % (completed.stderr.strip() or completed.stdout.strip()))
        os.replace(partial, output)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return output


def ensure_icns(exe_path, build_root):
    """The cached .icns for this executable, regenerating it when the image changed."""
    exe_path = Path(exe_path)
    cache = Path(build_root) / "macos-icon"
    icns = cache / "AppIcon.icns"
    stamp = cache / "source.sha256"
    digest = hashlib.sha256(exe_path.read_bytes()).hexdigest()
    token = "%d\n%s\n" % (CACHE_VERSION, digest)
    if icns.is_file() and stamp.is_file() and stamp.read_text() == token:
        return icns
    build_icns(exe_path, icns, work_dir=cache)
    stamp.write_text(token)
    return icns


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True, help="the game executable holding the icon")
    parser.add_argument("--build-root", type=Path, required=True,
                        help="build root; the .icns is cached under <build root>/macos-icon")
    parser.add_argument("--out", type=Path, default=None, help="also write the .icns here")
    args = parser.parse_args()
    icns = ensure_icns(args.exe, args.build_root)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(icns, args.out)
        icns = args.out
    print("icon: %s" % icns)


if __name__ == "__main__":
    sys.exit(main())
