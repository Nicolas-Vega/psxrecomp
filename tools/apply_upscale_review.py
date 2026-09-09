#!/usr/bin/env python3
"""Copies only the approved files (from generate_upscale_review.py's
"Export approved list" button) from the upscaled-output directory into the
HD texture pack root.

Usage:
    python apply_upscale_review.py --approved approved_textures.txt --after DIR --pack-dir PACK_DIR
"""
import argparse
import shutil
import sys
from pathlib import Path

DEFAULT_PACK_DIR = r"D:\RecompWork\Sources\Vagrant Story (USA)-texture-replacements"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--approved", required=True, help="approved_textures.txt from the review page")
    ap.add_argument("--after", required=True, help="Directory holding the upscaled PNGs")
    ap.add_argument("--pack-dir", default=DEFAULT_PACK_DIR, help="HD texture pack root")
    ap.add_argument("--overwrite", action="store_true", help="Overwrite an existing file in the pack root")
    args = ap.parse_args()

    approved_path = Path(args.approved)
    after_dir = Path(args.after)
    pack_dir = Path(args.pack_dir)

    names = [l.strip() for l in approved_path.read_text(encoding="utf-8").splitlines() if l.strip()]
    print(f"{len(names)} approved filename(s)")

    copied = missing = skipped = 0
    for name in names:
        src = after_dir / name
        dst = pack_dir / name
        if not src.is_file():
            print(f"  MISSING (not in --after): {name}", file=sys.stderr)
            missing += 1
            continue
        if dst.exists() and not args.overwrite:
            skipped += 1
            continue
        shutil.copy2(src, dst)
        copied += 1

    print(f"done: {copied} copied, {skipped} skipped (already in pack), {missing} missing from --after")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
