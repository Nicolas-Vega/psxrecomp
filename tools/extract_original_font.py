#!/usr/bin/env python3
"""Extract Vagrant Story's original in-game font bitmap from a disc image.

The retail font (vierock's HD texture pack replaces it with a stylized
gothic face) lives in SYSTEM.DAT as a 256x220 4bpp-indexed bitmap with a
16-color RGBA5551 CLUT, at fixed offsets within that file. This decodes it
straight from the user's own disc image and writes a plain RGBA PNG, for
use as the reference sheet by generate_font_pack.py (which slices it into
per-glyph cells and can regenerate the pack using any installed TrueType
font instead).

Offsets confirmed against https://github.com/HilltopWorks/VagrantStory-Font
(read as a black-box format reference only -- this is an independent
reimplementation of the standard PS1 raw-CD-sector / 4bpp-indexed /
RGBA5551 CLUT decode, not a copy of that repo's code).

Usage:
    python extract_original_font.py --bin "Vagrant Story (U) [SLUS-01040].bin" --out original_font.png
"""
import argparse
import struct

DATA_START = 24          # skip 12-byte sync + 4-byte header + 8-byte subheader
DATA_SECTOR_SIZE = 0x800  # usable bytes per MODE2/2352 sector
SECTOR_SIZE = 2352

# Fixed location within SYSTEM.DAT (sector 1387, 0x2e000 bytes long).
SYSTEM_DAT_SECTOR = 1387
SYSTEM_DAT_SIZE = 0x2E000
FONT_PXL_OFFSET = 0x1AA70
FONT_CLUT_OFFSET = 0x21970
FONT_WIDTH = 256
FONT_HEIGHT = 220


def extract_file(bin_path, start_sector, size_in_bytes):
    out = bytearray()
    with open(bin_path, "rb") as f:
        remaining = size_in_bytes
        sector = start_sector
        while remaining > 0:
            f.seek(sector * SECTOR_SIZE + DATA_START)
            chunk = min(remaining, DATA_SECTOR_SIZE)
            out += f.read(chunk)
            remaining -= chunk
            sector += 1
    return bytes(out)


def decode_4bit(data, offset, width, height):
    indices = []
    pos = offset
    for _y in range(height):
        for _x in range(width // 2):
            b = data[pos]
            pos += 1
            indices.append(b & 0x0F)
            indices.append((b >> 4) & 0x0F)
    return indices


def decode_clut_5551(data, offset, n_colors):
    colors = []
    for i in range(n_colors):
        val = struct.unpack_from("<H", data, offset + i * 2)[0]
        r5 = val & 0x1F
        g5 = (val >> 5) & 0x1F
        b5 = (val >> 10) & 0x1F
        stp = (val >> 15) & 1
        r = round(r5 * 255 / 31)
        g = round(g5 * 255 / 31)
        b = round(b5 * 255 / 31)
        a = 0 if (r5 == g5 == b5 == stp == 0) else 255
        colors.append((r, g, b, a))
    return colors


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", required=True, help="Path to the Vagrant Story disc image (.bin)")
    parser.add_argument("--out", required=True, help="Output PNG path for the decoded font sheet")
    args = parser.parse_args()

    from PIL import Image

    print(f"Extracting SYSTEM.DAT (sector {SYSTEM_DAT_SECTOR}, {SYSTEM_DAT_SIZE:#x} bytes)...")
    system_dat = extract_file(args.bin, SYSTEM_DAT_SECTOR, SYSTEM_DAT_SIZE)
    print("Got", len(system_dat), "bytes")

    indices = decode_4bit(system_dat, FONT_PXL_OFFSET, FONT_WIDTH, FONT_HEIGHT)
    clut = decode_clut_5551(system_dat, FONT_CLUT_OFFSET, 16)

    im = Image.new("RGBA", (FONT_WIDTH, FONT_HEIGHT), (0, 0, 0, 0))
    for y in range(FONT_HEIGHT):
        for x in range(FONT_WIDTH):
            im.putpixel((x, y), clut[indices[y * FONT_WIDTH + x]])
    im.save(args.out)
    print("Saved", args.out)


if __name__ == "__main__":
    main()
