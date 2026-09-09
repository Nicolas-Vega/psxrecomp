#!/usr/bin/env python3
"""Batch-upscales missing_textures/*.png (see hd_texture_pack.cpp's
hd_texture_pack_export_missing) with an ESRGAN-family model and drops the
results straight into the active HD texture pack root, ready to match on
the next launch -- filenames are already the pack's own <texhash>-<palhash>.png
convention, so no renaming is needed.

Requires (not vendored -- installed separately, see README below):
    pip install torch --index-url https://download.pytorch.org/whl/cpu   # or a CUDA index if you have an NVIDIA GPU
    pip install spandrel pillow numpy scipy
and an ESRGAN-architecture .pth checkpoint (tested against 4x_foolhardy_Remacri,
recommended for this pack's dark/architectural style -- see
https://openmodeldb.info/models/4x-Remacri).

Alpha handling: this pack's convention is a hard cutout (alpha 0 = fully
transparent, else opaque -- see gpu_gl_renderer.c's HD_FS, which discards
below 0.5 and never blends). Naively 4x-upscaling the source RGBA as-is
would let the AI model blend real colour against the (0,0,0,0) black used
for transparent pixels, leaving a dark halo at every cutout edge once the
alpha channel is remapped back to the same hard cutoff. Fixed the same way
the DuckStation upscaler linked above does: extend each opaque pixel's
colour into the fully-transparent region (nearest-neighbour fill) BEFORE
running it through the model, so the model only ever blends plausible
opaque colours -- then upscale the alpha channel separately with a nearest
resize (preserves the hard cutout exactly; this pack has no soft/AA edges
to preserve, and the render pipeline would discard/keep-whole any AI-
softened edge anyway under the current hard 0.5 cutoff).

Usage:
    python upscale_missing_textures.py --model PATH\\to\\4x_foolhardy_Remacri.pth [options]

Options:
    --pack-dir PATH   HD texture pack root (default: DEFAULT_PACK_DIR below)
    --limit N         process at most N files (0 = no limit, default)
    --dry-run         decode + upscale but do not write any output files
    --overwrite       overwrite an existing file in the pack root (default:
                      skip -- never clobbers a file someone already curated)
"""
import argparse
import sys
from pathlib import Path

DEFAULT_PACK_DIR = r"D:\RecompWork\Sources\Vagrant Story (USA)-texture-replacements"
UPSCALE_SCALE = 4  # matches 4x_foolhardy_Remacri; adjust if using a different-scale model


def extend_colors_into_transparent(rgba):
    """Fill fully-transparent pixels' RGB with the nearest fully-opaque
    pixel's RGB (nearest-neighbour fill via a Euclidean distance transform).
    Alpha is untouched. No-op (returns rgba unchanged) when the image is
    fully opaque or fully transparent -- both common in this pack."""
    import numpy as np
    from scipy.ndimage import distance_transform_edt

    alpha = rgba[..., 3]
    opaque = alpha > 0
    if opaque.all() or not opaque.any():
        return rgba
    _, indices = distance_transform_edt(~opaque, return_indices=True)
    out = rgba.copy()
    out[..., :3] = rgba[..., :3][indices[0], indices[1]]
    return out


def upscale_one(model, src_path, dst_path, scale, dry_run):
    import numpy as np
    import torch
    from PIL import Image

    im = Image.open(src_path).convert("RGBA")
    rgba = np.array(im)
    w, h = im.size

    filled = extend_colors_into_transparent(rgba)
    rgb = filled[..., :3].astype(np.float32) / 255.0
    tensor = torch.from_numpy(rgb).permute(2, 0, 1).unsqueeze(0)
    with torch.no_grad():
        out = model(tensor)
    out_rgb = out.squeeze(0).permute(1, 2, 0).clamp(0, 1).numpy()
    out_rgb = (out_rgb * 255.0).round().astype("uint8")

    out_h, out_w = out_rgb.shape[0], out_rgb.shape[1]
    alpha_img = Image.fromarray(rgba[..., 3], mode="L").resize(
        (out_w, out_h), Image.NEAREST
    )
    out_alpha = np.array(alpha_img)

    out_rgba = np.dstack([out_rgb, out_alpha])
    out_img = Image.fromarray(out_rgba, mode="RGBA")

    if not dry_run:
        dst_path.parent.mkdir(parents=True, exist_ok=True)
        out_img.save(dst_path)
    return (w, h), (out_w, out_h)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True, help="Path to an ESRGAN-architecture .pth checkpoint")
    ap.add_argument("--pack-dir", default=DEFAULT_PACK_DIR, help="HD texture pack root")
    ap.add_argument("--limit", type=int, default=0, help="Process at most N files (0 = no limit)")
    ap.add_argument("--dry-run", action="store_true", help="Upscale but do not write output files")
    ap.add_argument("--overwrite", action="store_true", help="Overwrite an existing pack-root file")
    args = ap.parse_args()

    pack_dir = Path(args.pack_dir)
    missing_dir = pack_dir / "missing_textures"
    if not missing_dir.is_dir():
        print(f"error: {missing_dir} not found", file=sys.stderr)
        return 1

    try:
        from spandrel import ImageModelDescriptor, ModelLoader
    except ImportError:
        print("error: spandrel not installed -- pip install spandrel pillow numpy scipy torch", file=sys.stderr)
        return 1

    model = ModelLoader().load_from_file(args.model)
    if not isinstance(model, ImageModelDescriptor):
        print(f"error: {args.model} is not a single-image super-resolution model", file=sys.stderr)
        return 1
    model.eval()
    print(f"loaded {Path(args.model).name}: scale={model.scale}, arch={model.architecture}")

    sources = sorted(missing_dir.glob("*.png"))
    if args.limit > 0:
        sources = sources[: args.limit]
    print(f"{len(sources)} candidate(s) in {missing_dir}")

    done = skipped_exists = failed = 0
    for src in sources:
        dst = pack_dir / src.name
        if dst.exists() and not args.overwrite:
            skipped_exists += 1
            continue
        try:
            in_size, out_size = upscale_one(model, src, dst, UPSCALE_SCALE, args.dry_run)
        except Exception as exc:  # noqa: BLE001 -- report and keep going, one bad file shouldn't stop the batch
            print(f"  FAILED {src.name}: {exc}", file=sys.stderr)
            failed += 1
            continue
        done += 1
        tag = "[dry-run] " if args.dry_run else ""
        print(f"  {tag}{src.name}: {in_size[0]}x{in_size[1]} -> {out_size[0]}x{out_size[1]}")

    print(f"done: {done} upscaled, {skipped_exists} skipped (already in pack), {failed} failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
