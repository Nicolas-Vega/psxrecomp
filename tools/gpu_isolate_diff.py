#!/usr/bin/env python3
"""gpu_isolate_diff.py -- deterministic-replay A/B, step 3+4.

See docs/internal/HD_RECOLOR_DETERMINISTIC_AB_CAPTURE_PLAN.md. Step 2
(gpu_replay_capture.py) proved two backends render byte-identical output
from byte-identical input, at whole-frame granularity. This isolates and
diffs ONE PRIMITIVE AT A TIME: for each drawing command in a captured
frame, both backends' OWN pixel contribution (real rasterization output,
diffed against a flat chroma-key background it was cleared to before that
primitive drew) is compared directly against the other backend's
contribution for that SAME primitive. That turns "is there a difference
somewhere in this frame" into a ranked, per-primitive list -- exactly the
`bcf02fd0` invisible-torso signature (content in one backend, nothing in
the other for the same primitive) is a maximum-severity "coverage" verdict
below, impossible to miss in a worst-first list the way it was buried in a
1526-pixel whole-frame diff.

    python3 gpu_isolate_diff.py analysis/captures/bad
    python3 gpu_isolate_diff.py analysis/captures/bad --backend-a 0 --backend-b 2 --top 20

Requires a bundle already captured by gpu_capture_state.py. Runs
gpu_replay_isolate ONCE, doing both backends inside that single atomic
call -- an earlier version called it twice (once per backend) and that was
wrong: between two separate round-trips the LIVE game gets to run real
frames, which can leave the GL renderer's internal batching/dirty-flag
state different for pass B than pass A saw, making the two backends'
results incomparable even against an unchanged, savestate-derived capture.
One call, no gap, both backends see identical starting conditions. Each
backend's isolate_manifest.json/iso_*.png land in their own `isolate_bN/`
subdirectory under the bundle. Then joins both manifests by `seq` (both
backends replay the identical captured word stream in the identical order,
so every primitive has the same seq in both) and reports, worst first:

  * "coverage: only A" / "coverage: only B" -- one backend drew real pixels
    for this primitive, the other's isolated crop is empty (still exactly
    background-colored) -- the torso-invisible bug shape.
  * "content differs" -- both backends drew something, but the pixels
    differ (tint/recolor-table bugs, wrong UV, etc.) -- reports differing
    pixel count and mean absolute color delta over the changed region.
  * "match" / "both blank" -- nothing suspicious about this primitive.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gpu_replay_capture import build_replay_words  # noqa: E402
from psx_gpu_frame import DEFAULT_NATIVE_PORT, DebugConn, DebugError  # noqa: E402


def load_manifest(out_dir: str) -> dict:
    with open(os.path.join(out_dir, "isolate_manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    manifest["_out_dir"] = out_dir
    return manifest


def _load_rgb(path):
    from PIL import Image
    return Image.open(path).convert("RGB")


def compare_entry(entry_a: dict, entry_b: dict, out_dir_a: str, out_dir_b: str,
                  chroma) -> dict:
    """One primitive's cross-backend verdict. `entry_a`/`entry_b` are the
    matching (by seq) isolate_manifest.json entries."""
    file_a, file_b = entry_a.get("file"), entry_b.get("file")
    base = {"seq": entry_a["seq"], "op": entry_a["op"], "index_a": entry_a["index"],
            "index_b": entry_b["index"]}

    if file_a is None and file_b is None:
        return {**base, "verdict": "both blank", "score": 0}
    if file_a is None or file_b is None:
        which = "B" if file_a is None else "A"
        return {**base, "verdict": f"coverage: only {which} drew",
                "score": float("inf"),
                "changed_px_a": entry_a.get("changed_px", 0),
                "changed_px_b": entry_b.get("changed_px", 0)}

    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        return {**base, "verdict": "both drew (install pillow+numpy to compare pixels)",
                "score": 1}

    bbox_a, bbox_b = entry_a["bbox"], entry_b["bbox"]
    ux0, uy0 = min(bbox_a[0], bbox_b[0]), min(bbox_a[1], bbox_b[1])
    ux1, uy1 = max(bbox_a[2], bbox_b[2]), max(bbox_a[3], bbox_b[3])
    uw, uh = ux1 - ux0 + 1, uy1 - uy0 + 1

    canvas_a = np.full((uh, uw, 3), chroma, dtype=np.int16)
    canvas_b = np.full((uh, uw, 3), chroma, dtype=np.int16)
    a_img = np.asarray(_load_rgb(os.path.join(out_dir_a, file_a)), dtype=np.int16)
    b_img = np.asarray(_load_rgb(os.path.join(out_dir_b, file_b)), dtype=np.int16)
    canvas_a[bbox_a[1]-uy0:bbox_a[1]-uy0+a_img.shape[0],
             bbox_a[0]-ux0:bbox_a[0]-ux0+a_img.shape[1]] = a_img
    canvas_b[bbox_b[1]-uy0:bbox_b[1]-uy0+b_img.shape[0],
             bbox_b[0]-ux0:bbox_b[0]-ux0+b_img.shape[1]] = b_img

    diff = np.abs(canvas_a - canvas_b).sum(axis=2)
    changed = diff > 0
    n_changed = int(changed.sum())
    if n_changed == 0:
        return {**base, "verdict": "match", "score": 0, "bbox": [ux0, uy0, ux1, uy1]}
    return {**base, "verdict": "content differs", "score": float(diff[changed].mean()),
            "changed_px": n_changed, "bbox": [ux0, uy0, ux1, uy1]}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="a bundle directory written by gpu_capture_state.py")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="isolation replays the whole frame once per primitive; "
                         "busy frames need a longer timeout than a plain capture")
    ap.add_argument("--backend-a", type=int, default=0)
    ap.add_argument("--backend-b", type=int, default=2)
    ap.add_argument("--chroma", default="255,0,255", help="R,G,B chroma-key color")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--json", default=None, help="also write the full ranked diff as JSON")
    args = ap.parse_args(argv)

    chroma = tuple(int(c) for c in args.chroma.split(","))
    bundle_dir = os.path.abspath(args.dir)
    gp0_stream_path = os.path.join(bundle_dir, "gp0_stream.json")
    words_bin_path = os.path.join(bundle_dir, "replay_words.bin")
    if not os.path.isfile(gp0_stream_path):
        print(f"error: {gp0_stream_path} not found -- run gpu_capture_state.py first",
              file=sys.stderr)
        return 2
    report = build_replay_words(gp0_stream_path, words_bin_path)
    print(f"replay_words.bin: {report['kept']}/{report['total']} entries kept "
          f"({report['dropped_truncated']} truncated, "
          f"{report['dropped_transfer']} VRAM-transfer opcodes, "
          f"{report['dropped_polyline']} polyline headers dropped)")

    out_dir_a = os.path.join(bundle_dir, f"isolate_b{args.backend_a}")
    out_dir_b = os.path.join(bundle_dir, f"isolate_b{args.backend_b}")
    os.makedirs(out_dir_a, exist_ok=True)
    os.makedirs(out_dir_b, exist_ok=True)

    try:
        with DebugConn(args.host, args.port, args.timeout) as conn:
            print(f"isolating backend {args.backend_a} vs {args.backend_b} "
                  f"(one atomic call, no gap between them)...")
            r = conn.cmd("gpu_replay_isolate", dir=bundle_dir,
                        out_dir_a=out_dir_a, out_dir_b=out_dir_b,
                        backend_a=args.backend_a, backend_b=args.backend_b,
                        chroma_r=chroma[0], chroma_g=chroma[1], chroma_b=chroma[2])
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    print(f"  backend {r['backend_a']}: {r['total_drawing_a']} drawing entries, "
          f"{r['wrote_files_a']} produced a crop")
    print(f"  backend {r['backend_b']}: {r['total_drawing_b']} drawing entries, "
          f"{r['wrote_files_b']} produced a crop")
    man_a = load_manifest(out_dir_a)
    man_b = load_manifest(out_dir_b)

    by_seq_b = {e["seq"]: e for e in man_b["entries"]}
    results = []
    for entry_a in man_a["entries"]:
        entry_b = by_seq_b.get(entry_a["seq"])
        if entry_b is None:
            results.append({"seq": entry_a["seq"], "op": entry_a["op"],
                            "verdict": "no matching entry in backend B", "score": float("inf")})
            continue
        results.append(compare_entry(entry_a, entry_b, man_a["_out_dir"], man_b["_out_dir"], chroma))

    # Worst first: "inf" (coverage bugs) before any finite score, then by
    # descending score within each group.
    results.sort(key=lambda r: (r["score"] != float("inf"),
                                -r["score"] if r["score"] != float("inf") else 0))

    verdict_counts = {}
    for r in results:
        verdict_counts[r["verdict"]] = verdict_counts.get(r["verdict"], 0) + 1
    print(f"\n{len(results)} primitive(s) compared:")
    for v, n in sorted(verdict_counts.items(), key=lambda kv: -kv[1]):
        print(f"  {n:>4}  {v}")

    print(f"\nworst {min(args.top, len(results))} (by score):")
    print(f"  {'seq':>6} {'op':<6} {'verdict':<28} {'score':>10}")
    for r in results[:args.top]:
        score_str = "inf" if r["score"] == float("inf") else f"{r['score']:.1f}"
        print(f"  {r['seq']:>6} {r['op']:<6} {r['verdict']:<28} {score_str:>10}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"backend_a": args.backend_a, "backend_b": args.backend_b,
                      "out_dir_a": man_a["_out_dir"], "out_dir_b": man_b["_out_dir"],
                      "results": results}, f, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
