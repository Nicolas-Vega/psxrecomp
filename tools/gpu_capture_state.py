#!/usr/bin/env python3
"""gpu_capture_state.py -- deterministic-replay A/B capture, step 1.

See docs/internal/HD_RECOLOR_DETERMINISTIC_AB_CAPTURE_PLAN.md for the full
plan this is step 1 of. Where gpu_frame_capture.py captures a frame's
decoded primitive stream for attribution (which guest function drew what),
this captures everything a later REPLAY needs to reproduce that frame's
GPU input byte-for-byte, once, so it can be fed through both rendering
backends deterministically instead of comparing two live captures taken at
different moments (see the psx-deterministic-replay-ab-over-live-burst-median
memory for why that matters).

    # capture the newest frame the ring holds
    python3 gpu_capture_state.py --tag bad --out analysis/captures

    # a specific past frame
    python3 gpu_capture_state.py --frame 41230 --tag bad --out analysis/captures

One call to the runtime's `gpu_capture_state` debug command writes, all from
a single synchronous handler (so the bundle is internally consistent -- the
game never gets a chance to advance between the VRAM read, the upload-
tracker read, and the GP0 ring read):

    vram.bin        raw 1024x512 uint16 VRAM mirror, row-major, little-endian
    uploads.bin     the HD texture pack's upload-tracking wire blob (what the
                     HD matcher currently believes is resident where -- the
                     state a savestate does NOT carry, see
                     psx-recomp-render-ab-testing-methodology)
    gp0_stream.json this frame's GP0 ring entries (same schema as
                     gpu_frame_dump's response, plus a "truncated" flag)
    screenshot.png   reference image (only written for the live/newest frame)
    meta.json        counts, truncation flags, hd_backend, a format note

gp0_stream.json includes VRAM-transfer opcodes (0x80/0xA0/0xC0) verbatim for
attribution, but their payload beyond the ring's word cap is NOT captured
there -- a replay must source VRAM content from vram.bin, never by replaying
those opcodes from this stream. Only the small drawing/state-setting
opcodes are meant to be replayed through gpu_write_gp0.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import DEFAULT_NATIVE_PORT, DebugConn, DebugError  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--frame", type=int, default=None,
                    help="frame to capture (default: the newest one the ring holds)")
    ap.add_argument("--ring", action="store_true",
                    help="print which frames the GP0 ring can still be asked "
                         "for, then exit")
    ap.add_argument("--tag", default="capture", help="output subdirectory name")
    ap.add_argument("--out", default="analysis/captures",
                    help="parent directory -- the bundle is written to <out>/<tag>/")
    args = ap.parse_args(argv)

    try:
        with DebugConn(args.host, args.port, args.timeout) as conn:
            span = conn.ring_span()
            if args.ring:
                print(f"GP0 ring: {span['total']} packet(s) seen, capacity "
                      f"{span['capacity']}, {span['max_words']} words/packet")
                if span["total"] == 0:
                    print("  nothing recorded yet -- is a game running?")
                else:
                    print(f"  capturable frames: {span['oldest']}..{span['newest']}")
                return 0

            frame = args.frame if args.frame is not None else span["newest"]
            if frame < span["oldest"] or frame > span["newest"]:
                print(f"warning: frame {frame} is outside the ring's current "
                      f"range ({span['oldest']}..{span['newest']}); capturing "
                      f"anyway, expect an empty gp0_stream", file=sys.stderr)

            bundle_dir = os.path.abspath(os.path.join(args.out, args.tag))
            os.makedirs(bundle_dir, exist_ok=True)

            r = conn.cmd("gpu_capture_state", dir=bundle_dir, frame=frame)

            print(f"wrote {bundle_dir}")
            print(f"  frame {r['frame']}  ring={r['ring_oldest']}..{r['ring_newest']}")
            print(f"  gp0_entries={r['gp0_entries']}"
                  + (f"  ({r['gp0_truncated_entries']} truncated to the ring's "
                     f"word cap)" if r.get("gp0_truncated_entries") else ""))
            print(f"  vram_bytes={r['vram_bytes']}")
            print(f"  uploads_bytes={r['uploads_bytes']}  ok={r['uploads_ok']}")
            print(f"  screenshot={'yes' if r['has_screenshot'] else 'no'}")

            meta_path = os.path.join(bundle_dir, "meta.json")
            if os.path.isfile(meta_path):
                with open(meta_path, encoding="utf-8") as f:
                    meta = json.load(f)
                print(f"  hd_backend={meta.get('hd_backend')}")
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
