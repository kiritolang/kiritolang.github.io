#!/usr/bin/env python3
"""Cross-validate the Kirito `imaging` package against Pillow (PIL).

The Kirito side (`_compare_gen.ki`) writes a base image and a set of operation results as PNG/PPM/BMP
files. This harness re-computes the same operations with Pillow and asserts the Kirito output matches
pixel-for-pixel (with a tiny tolerance only for the greyscale luma rounding, which differs by at most
one count between libraries). It also round-trips PIL-authored PNGs back through Kirito.

Usage:
    python3 compare_pillow.py [path/to/ki]
The `ki` interpreter defaults to ../../../build-release/ki relative to this file.
"""
import os
import subprocess
import sys

try:
    from PIL import Image
except ImportError:
    print("SKIP: Pillow not installed (pip install pillow) -- cross-validation skipped")
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
KI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "build-release", "ki")
GEN = os.path.join(HERE, "_compare_gen.ki")

failures = []


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}{(' -- ' + detail) if detail and not cond else ''}")
    if not cond:
        failures.append(name)


def max_diff(a, b):
    """Largest absolute per-channel difference between two same-size PIL images."""
    pa, pb = a.load(), b.load()
    if a.size != b.size or a.mode != b.mode:
        return 1 << 30
    w, h = a.size
    m = 0
    for y in range(h):
        for x in range(w):
            va, vb = pa[x, y], pb[x, y]
            if isinstance(va, int):
                va, vb = (va,), (vb,)
            for ca, cb in zip(va, vb):
                m = max(m, abs(ca - cb))
    return m


def main():
    # 1. Run the Kirito generator; its last stdout line is the output directory.
    res = subprocess.run([KI, GEN], capture_output=True, text=True)
    if res.returncode != 0:
        print("generator failed:\n", res.stderr)
        sys.exit(1)
    outdir = res.stdout.strip().splitlines()[-1]
    print("Kirito wrote reference images to", outdir)

    def kimg(name):
        return Image.open(os.path.join(outdir, name))

    base = kimg("base.png")
    print("base:", base.mode, base.size)
    W, Hh = base.size

    # 2. Kirito's operations vs Pillow's, on Pillow's reading of Kirito's base (same input both sides).
    print("Kirito op  vs  Pillow op:")
    check("nearest 2x", max_diff(kimg("nearest2x.png"),
                                 base.resize((W * 2, Hh * 2), Image.NEAREST)) == 0)
    check("flip left-right", max_diff(kimg("fliplr.png"),
                                      base.transpose(Image.FLIP_LEFT_RIGHT)) == 0)
    check("flip top-bottom", max_diff(kimg("fliptb.png"),
                                      base.transpose(Image.FLIP_TOP_BOTTOM)) == 0)
    check("rotate 90", max_diff(kimg("rot90.png"), base.transpose(Image.ROTATE_90)) == 0)
    check("rotate 180", max_diff(kimg("rot180.png"), base.transpose(Image.ROTATE_180)) == 0)
    check("rotate 270", max_diff(kimg("rot270.png"), base.transpose(Image.ROTATE_270)) == 0)
    check("transpose", max_diff(kimg("transpose.png"), base.transpose(Image.TRANSPOSE)) == 0)
    check("crop", max_diff(kimg("crop.png"), base.crop((2, 1, 10, 8))) == 0)
    check("RGB->L (tol 1)", max_diff(kimg("gray.png"), base.convert("L")) <= 1)
    check("RGB->RGBA", max_diff(kimg("rgba.png"), base.convert("RGBA")) == 0)

    # 3. Other codecs read back identically to the PNG base.
    print("Codec round-trips (PPM/BMP read by Pillow == base):")
    check("PPM == base", max_diff(kimg("base.ppm").convert("RGB"), base) == 0)
    check("BMP == base", max_diff(kimg("base.bmp").convert("RGB"), base) == 0)

    # 4. The reverse direction: Pillow writes adaptively-filtered PNGs; Kirito reads them back.
    print("Pillow -> Kirito PNG decode (adaptive filters):")
    pil = Image.new("RGB", (16, 12))
    for y in range(12):
        for x in range(16):
            pil.putpixel((x, y), ((x * 16) % 256, (y * 20) % 256, (x * y) % 256))
    rt = os.path.join(outdir, "_pil_round")
    os.makedirs(rt, exist_ok=True)
    for mode in ("RGB", "RGBA", "L"):
        pil.convert(mode).save(os.path.join(rt, f"pil_{mode}.png"))
    # A Kirito snippet decodes each and re-saves as PPM (lossless) for PIL to compare.
    snippet = (
        'var Image = import("imaging")\n'
        'var io = import("io")\n'
        'var path = import("path")\n'
        'for m in ["RGB", "RGBA", "L"]:\n'
        f'    var p = path.join("{rt}", "pil_" + m + ".png")\n'
        '    var im = Image.open(p)\n'
        f'    im.save(path.join("{rt}", "ki_" + m + ".ppm"))\n'
        'io.print("done")\n'
    )
    snip_path = os.path.join(rt, "_decode.ki")
    with open(snip_path, "w") as f:
        f.write(snippet)
    r2 = subprocess.run([KI, snip_path], capture_output=True, text=True)
    if r2.returncode != 0:
        print("decode snippet failed:\n", r2.stderr)
        failures.append("pil->ki decode run")
    else:
        for mode in ("RGB", "RGBA", "L"):
            ki_back = Image.open(os.path.join(rt, f"ki_{mode}.ppm")).convert(mode if mode != "RGBA" else "RGB")
            ref = pil.convert(mode).convert(mode if mode != "RGBA" else "RGB")
            check(f"decode PIL {mode}", max_diff(ki_back, ref) == 0)

    print()
    if failures:
        print(f"CROSS-VALIDATION FAILED: {len(failures)} mismatch(es): {failures}")
        sys.exit(1)
    print("CROSS-VALIDATION PASSED -- Kirito imaging matches Pillow")


if __name__ == "__main__":
    main()
