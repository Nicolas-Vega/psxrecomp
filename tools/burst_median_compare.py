"""One-off tool: capture N frames of the presented output under each HD
backend (holding the SAME savestate/scene), compute the per-pixel MEDIAN
across each burst (cancels PS1-style per-frame vertex/animation jitter that
a single-frame diff can't distinguish from a real, systematic shift), then
report the shift that best aligns the two median images.

Usage: python burst_median_compare.py <savestate_slot> <frame_count>
"""
import json
import socket
import sys
import time

import numpy as np
from PIL import Image

HOST = "127.0.0.1"
PORT = 4370


def send_cmd(sock_unused, cmd_dict):
    # The debug server closes the connection after each command (confirmed
    # live: reusing one socket aborts on the second send), so connect fresh
    # every call -- matches debug_client.py's REPL, which reconnects on
    # ConnectionResetError rather than keeping one socket across commands.
    s = socket.create_connection((HOST, PORT), timeout=10.0)
    payload = (json.dumps(cmd_dict) + "\n").encode()
    s.sendall(payload)
    buf = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            try:
                depth = 0
                for i, b in enumerate(buf):
                    c = chr(b)
                    if c == "{":
                        depth += 1
                    elif c == "}":
                        depth -= 1
                        if depth == 0:
                            return json.loads(buf[: i + 1].decode())
            except Exception:
                continue
    finally:
        s.close()
    return None


def capture_burst(sock_unused, count, out_prefix):
    frames = None
    for i in range(count):
        resp = send_cmd(None, {"cmd": "screenshot", "path": f"{out_prefix}_{i:04d}.png"})
        if not resp or not resp.get("ok"):
            print("capture failed at", i, resp)
            continue
        img = np.array(Image.open(f"{out_prefix}_{i:04d}.png").convert("RGB"), dtype=np.uint8)
        if frames is None:
            frames = np.empty((count,) + img.shape, dtype=np.uint8)
        frames[i] = img
        if (i + 1) % 60 == 0:
            print(f"  {i+1}/{count} captured")
    return frames


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 480

    # NO savestate load here: pack->uploads (which VRAM regions are tracked,
    # with content hashes) is runtime-only tracking state, not part of a
    # savestate -- confirmed live (2026-09-09) that jumping straight into a
    # savestate on a fresh process leaves it empty (matched:0 the whole
    # burst), silently comparing native-vs-native. The scene must be reached
    # by real play in the CURRENT process, then both bursts captured back to
    # back by toggling hd_backend live (which does NOT reset tracking) --
    # never reloading a savestate between them.
    print("Capturing backend=beetle burst (assumes already there via real play)...")
    resp = send_cmd(None, {"cmd": "hd_backend", "backend": "beetle"})
    m0 = resp["beetle"]["match_stats"]["matched"]
    frames_beetle = capture_burst(None, count, "burst_beetle")
    median_beetle = np.median(frames_beetle, axis=0).astype(np.float64)
    Image.fromarray(median_beetle.astype(np.uint8)).save("median_beetle.png")
    resp = send_cmd(None, {"cmd": "hd_backend"})
    m1 = resp["beetle"]["match_stats"]["matched"]
    print(f"  matched count during burst: {m0} -> {m1} (delta {m1 - m0})")
    if m1 - m0 < count:
        print("  WARNING: fewer new matches than frames captured -- verify tracking is really live!")

    print("Switching to backend=none, capturing burst...")
    send_cmd(None, {"cmd": "hd_backend", "backend": "none"})
    time.sleep(0.2)
    frames_none = capture_burst(None, count, "burst_none")
    median_none = np.median(frames_none, axis=0).astype(np.float64)
    Image.fromarray(median_none.astype(np.uint8)).save("median_none.png")

    print("\nShift search (whole frame):")
    a, b = median_none, median_beetle
    best = None
    for dx in range(-10, 11):
        if dx >= 0:
            aa, bb = a[:, dx:], b[:, : a.shape[1] - dx]
        else:
            aa, bb = a[:, :dx], b[:, -dx:]
        d = np.abs(aa - bb).sum()
        if best is None or d < best[1]:
            best = (dx, d)
        print(dx, d)
    print("BEST SHIFT:", best)


if __name__ == "__main__":
    main()
