#!/usr/bin/env python3
"""Cross-validate the Kirito `video` module (and its jpeg/gif codecs) against Pillow.

Generates JPEG / animated-GIF / MJPEG / Y4M / image-sequence assets with Pillow, has the Kirito
`VideoCapture` decode each (via _video_dump.ki), and compares pixel-for-pixel. GIF and the image
sequence must match exactly; JPEG/MJPEG/Y4M are lossy/round-tripped, so a small tolerance applies.
Also stands up a local multipart/x-mixed-replace MJPEG HTTP server and checks the network backend.

    pip install pillow
    python3 compare_video_pillow.py [path/to/ki]
"""
import io as _io
import os
import socket
import subprocess
import sys
import threading

try:
    from PIL import Image, ImageSequence
except ImportError:
    print("SKIP: Pillow not installed")
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
KI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "build-release", "ki")
DUMP = os.path.join(HERE, "_video_dump.ki")
OUT = "/tmp/ki_video_xval"
os.makedirs(OUT, exist_ok=True)
failures = []


def check(name, cond, detail=""):
    print(f"  [{'ok' if cond else 'FAIL'}] {name}{('  ' + detail) if not cond else ''}")
    if not cond:
        failures.append(name)


def maxdiff(a, b):
    if a.size != b.size:
        return 1 << 30
    pa, pb = a.convert("RGB").load(), b.convert("RGB").load()
    m = 0
    for y in range(a.size[1]):
        for x in range(a.size[0]):
            for u, v in zip(pa[x, y], pb[x, y]):
                m = max(m, abs(u - v))
    return m


def dump(src, prefix, maxf=64, extra_env=None):
    r = subprocess.run([KI, DUMP, src, prefix, str(maxf)], capture_output=True, text=True,
                       cwd=HERE, env=extra_env)
    if r.returncode != 0:
        print("   dump failed:", r.stderr.strip()[:400])
        return -1
    return int(r.stdout.strip().splitlines()[-1])


def make_frames(n, w, h):
    out = []
    for t in range(n):
        im = Image.new("RGB", (w, h))
        for y in range(h):
            for x in range(w):
                im.putpixel((x, y), (((x + t * 4) * 13) % 256, (y * 17) % 256, (t * 50) % 256))
        out.append(im)
    return out


def main():
    # ---- MJPEG file (4:4:4 -> near exact) ----
    frames = make_frames(4, 24, 18)
    mjpeg = os.path.join(OUT, "clip.mjpeg")
    with open(mjpeg, "wb") as f:
        for im in frames:
            b = _io.BytesIO(); im.save(b, "JPEG", quality=95, subsampling=0); f.write(b.getvalue())
    n = dump(mjpeg, os.path.join(OUT, "mj_"), 64)
    check("mjpeg frame count", n == 4, f"got {n}")
    worst = 0
    for i in range(max(n, 0)):
        worst = max(worst, maxdiff(frames[i], Image.open(os.path.join(OUT, f"mj_{i}.ppm"))))
    check("mjpeg pixels (tol 5)", worst <= 5, f"maxdiff {worst}")

    # ---- animated GIF (exact) ----
    gpath = os.path.join(OUT, "anim.gif")
    frames[0].save(gpath, save_all=True, append_images=frames[1:], duration=100, loop=0)
    n = dump(gpath, os.path.join(OUT, "gf_"), 64)
    g = Image.open(gpath)
    check("gif frame count", n == g.n_frames, f"{n} vs {g.n_frames}")
    worst = 0
    for i, fr in enumerate(ImageSequence.Iterator(g)):
        worst = max(worst, maxdiff(fr, Image.open(os.path.join(OUT, f"gf_{i}.ppm"))))
    check("gif pixels (exact)", worst == 0, f"maxdiff {worst}")

    # ---- image sequence (exact) ----
    seqdir = os.path.join(OUT, "seq"); os.makedirs(seqdir, exist_ok=True)
    for i, im in enumerate(frames):
        im.save(os.path.join(seqdir, f"f{i:04d}.png"))
    n = dump(os.path.join(seqdir, "f%04d.png"), os.path.join(OUT, "sq_"), 64)
    check("seq frame count", n == 4, f"got {n}")
    worst = max((maxdiff(frames[i], Image.open(os.path.join(OUT, f"sq_{i}.ppm"))) for i in range(max(n, 0))), default=99)  # noqa: E501
    check("seq pixels (exact)", worst == 0, f"maxdiff {worst}")

    # ---- Y4M (C444, JFIF full-range) round-trip ----
    def clamp(v): return max(0, min(255, int(round(v))))
    W, H = 24, 18
    y4m = os.path.join(OUT, "clip.y4m")
    refs = []
    with open(y4m, "wb") as out:
        out.write(f"YUV4MPEG2 W{W} H{H} F25:1 Ip A1:1 C444\n".encode())
        for im in frames:
            Y = bytearray(); Cb = bytearray(); Cr = bytearray()
            for y in range(H):
                for x in range(W):
                    r, g, b = im.getpixel((x, y))
                    Y.append(clamp(0.299 * r + 0.587 * g + 0.114 * b))
                    Cb.append(clamp(128 - 0.168736 * r - 0.331264 * g + 0.5 * b))
                    Cr.append(clamp(128 + 0.5 * r - 0.418688 * g - 0.081312 * b))
            out.write(b"FRAME\n"); out.write(Y); out.write(Cb); out.write(Cr); refs.append(im)
    n = dump(y4m, os.path.join(OUT, "y4_"), 64)
    check("y4m frame count", n == 4, f"got {n}")
    worst = max((maxdiff(refs[i], Image.open(os.path.join(OUT, f"y4_{i}.ppm"))) for i in range(max(n, 0))), default=99)
    check("y4m pixels (tol 5)", worst <= 5, f"maxdiff {worst}")

    # ---- network MJPEG (multipart/x-mixed-replace) over a local server ----
    jpegs = []
    for im in frames:
        b = _io.BytesIO(); im.save(b, "JPEG", quality=95, subsampling=0); jpegs.append(b.getvalue())
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0)); srv.listen(1)
    port = srv.getsockname()[1]

    def serve():
        conn, _ = srv.accept()
        conn.recv(4096)
        bnd = b"kiriframe"
        conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=" + bnd + b"\r\n\r\n")
        for jp in jpegs:
            conn.sendall(b"--" + bnd + b"\r\nContent-Type: image/jpeg\r\nContent-Length: "
                         + str(len(jp)).encode() + b"\r\n\r\n" + jp + b"\r\n")
        conn.close(); srv.close()

    threading.Thread(target=serve, daemon=True).start()
    n = dump(f"http://127.0.0.1:{port}/stream", os.path.join(OUT, "ht_"), 4)
    check("http mjpeg frame count", n == 4, f"got {n}")
    worst = max((maxdiff(frames[i], Image.open(os.path.join(OUT, f"ht_{i}.ppm"))) for i in range(max(n, 0))), default=99)  # noqa: E501
    check("http mjpeg pixels (tol 5)", worst <= 5, f"maxdiff {worst}")

    print()
    if failures:
        print(f"VIDEO CROSS-VALIDATION FAILED: {failures}")
        sys.exit(1)
    print("VIDEO CROSS-VALIDATION PASSED -- Kirito video matches Pillow")


if __name__ == "__main__":
    main()
