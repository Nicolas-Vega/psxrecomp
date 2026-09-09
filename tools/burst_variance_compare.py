"""One-off tool: capture N frames of the presented output under each HD
backend (same savestate/scene, same live-toggle rules as burst_median_
compare.py), and compute the per-pixel STANDARD DEVIATION across each burst
-- i.e. how much a pixel wobbles frame-to-frame, not just its average color.
Built to test a specific claim: that a gap/silhouette boundary jitters much
more under one backend than the other, which a median (which cancels jitter
out) can't distinguish from a perfectly stable pixel.

Usage: python burst_variance_compare.py <frame_count>
"""
import json
import socket
import sys
import time

import numpy as np
from PIL import Image

HOST = "127.0.0.1"
PORT = 4370


def send_cmd(cmd_dict):
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
            depth = 0
            for i, b in enumerate(buf):
                c = chr(b)
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        return json.loads(buf[: i + 1].decode())
    finally:
        s.close()
    return None


def capture_burst(count, out_prefix):
    frames = None
    for i in range(count):
        resp = send_cmd({"cmd": "screenshot", "path": f"{out_prefix}_{i:04d}.png"})
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
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 300

    print("Capturing backend=beetle burst (assumes already there via real play)...")
    send_cmd({"cmd": "hd_backend", "backend": "beetle"})
    frames_beetle = capture_burst(count, "burstvar_beetle")
    std_beetle = frames_beetle.astype(np.float64).std(axis=0)
    Image.fromarray(np.clip(std_beetle * 8, 0, 255).astype(np.uint8)).save("stddev_beetle_x8.png")
    np.save("stddev_beetle.npy", std_beetle)

    print("Switching to backend=none, capturing burst...")
    send_cmd({"cmd": "hd_backend", "backend": "none"})
    time.sleep(0.2)
    frames_none = capture_burst(count, "burstvar_none")
    std_none = frames_none.astype(np.float64).std(axis=0)
    Image.fromarray(np.clip(std_none * 8, 0, 255).astype(np.uint8)).save("stddev_none_x8.png")
    np.save("stddev_none.npy", std_none)

    print("\nPer-channel mean stddev (whole frame):")
    print("  none:  ", std_none.mean(axis=(0, 1)))
    print("  beetle:", std_beetle.mean(axis=(0, 1)))
    print("Max stddev (whole frame):")
    print("  none:  ", std_none.max())
    print("  beetle:", std_beetle.max())


if __name__ == "__main__":
    main()
