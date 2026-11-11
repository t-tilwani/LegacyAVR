"""
LegacyAVR - regenerate the app icons from the logo geometry (v1.6.0).

Only needed when the logo changes; the build uses the committed .ico files.
Requires Python 3.10+ and Pillow (py -m pip install pillow). Run from the repository root:

    py scripts\\make-icons.py

Reads the "AVR" path data from ui\\LegacyAVR.UI\\Services\\BrandLogo.cs (the single source of the
logo geometry) and writes:
    ui\\LegacyAVR.UI\\Assets\\LegacyAVR.ico        brand tile (#731012) with a white "AVR"
    ui\\LegacyAVR.UI\\Assets\\LegacyAVR-muted.ico  grey tile, dimmed "AVR" and a slash (Mute All)
Each .ico holds 16, 20, 24, 32, 40, 48 and 64 px images (32-bit DIB) and a 256 px PNG, so Windows
picks a sharp image for the title bar, taskbar, tray, Start menu, Explorer and the installer at
every DPI. Each size is rendered separately (16x supersampled), not scaled from one bitmap.
"""
import io
import os
import re
import struct
import sys

from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LOGO_CS = os.path.join(ROOT, "ui", "LegacyAVR.UI", "Services", "BrandLogo.cs")
ASSETS = os.path.join(ROOT, "ui", "LegacyAVR.UI", "Assets")

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SUPER = 16
BRAND = (0x73, 0x10, 0x12, 255)
MUTED_TILE = (0x3A, 0x3A, 0x40, 255)
MUTED_GLYPH = (0xB9, 0xB5, 0xB8, 255)
WHITE = (255, 255, 255, 255)
GLYPH_WIDTH = 0.78      # share of the tile width taken by "AVR"
CORNER = 0.22           # tile corner radius as a share of the size


def read_path(name):
    src = open(LOGO_CS, encoding="utf-8").read()
    m = re.search(name + r'\s*=\s*"([^"]+)"', src)
    if not m:
        sys.exit(f"{name} not found in {LOGO_CS}")
    return m.group(1)


def subpaths(data):
    """Absolute M/L/C/Z path data (as written by BrandLogo.cs) -> list of polygons."""
    tokens = data.split()
    polys, cur, i, pos = [], [], 0, (0.0, 0.0)
    while i < len(tokens):
        cmd = tokens[i]
        i += 1
        if cmd == "M":
            if cur:
                polys.append(cur)
            pos = (float(tokens[i]), float(tokens[i + 1]))
            i += 2
            cur = [pos]
        elif cmd == "L":
            pos = (float(tokens[i]), float(tokens[i + 1]))
            i += 2
            cur.append(pos)
        elif cmd == "C":
            p1 = (float(tokens[i]), float(tokens[i + 1]))
            p2 = (float(tokens[i + 2]), float(tokens[i + 3]))
            p3 = (float(tokens[i + 4]), float(tokens[i + 5]))
            i += 6
            for k in range(1, 17):
                t = k / 16
                u = 1 - t
                cur.append((u ** 3 * pos[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                            u ** 3 * pos[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
            pos = p3
        elif cmd == "Z":
            if cur:
                polys.append(cur)
            cur = []
        else:
            sys.exit(f"unexpected path command {cmd!r}")
    if cur:
        polys.append(cur)
    return polys


def bounds(polys):
    xs = [p[0] for poly in polys for p in poly]
    ys = [p[1] for poly in polys for p in poly]
    return min(xs), min(ys), max(xs), max(ys)


def render(size, polys, muted):
    big = size * SUPER
    tile = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(tile)
    d.rounded_rectangle((0, 0, big - 1, big - 1), radius=int(big * CORNER), fill=MUTED_TILE if muted else BRAND)

    # Glyph mask: each sub-path toggles coverage (even-odd), which keeps the counters of A and R open.
    x0, y0, x1, y1 = bounds(polys)
    scale = big * GLYPH_WIDTH / (x1 - x0)
    ox = (big - (x1 - x0) * scale) / 2 - x0 * scale
    oy = (big - (y1 - y0) * scale) / 2 - y0 * scale
    mask = Image.new("1", (big, big), 0)
    for poly in polys:
        sub = Image.new("1", (big, big), 0)
        ImageDraw.Draw(sub).polygon([(ox + x * scale, oy + y * scale) for x, y in poly], fill=1)
        mask = Image.frombytes("1", (big, big), bytes(a ^ b for a, b in zip(mask.tobytes(), sub.tobytes())))
    glyph = Image.new("RGBA", (big, big), MUTED_GLYPH if muted else WHITE)
    tile.paste(glyph, (0, 0), mask.convert("L"))

    if muted:
        w = int(big * 0.09)
        d = ImageDraw.Draw(tile)
        d.line((big * 0.2, big * 0.8, big * 0.8, big * 0.2), fill=MUTED_TILE, width=w + int(big * 0.08))
        d.line((big * 0.2, big * 0.8, big * 0.8, big * 0.2), fill=WHITE, width=w)

    # Re-apply the rounded-tile alpha so the slash outline cannot spill past the corners.
    alpha = Image.new("L", (big, big), 0)
    ImageDraw.Draw(alpha).rounded_rectangle((0, 0, big - 1, big - 1), radius=int(big * CORNER), fill=255)
    tile.putalpha(Image.composite(tile.getchannel("A"), alpha, alpha))
    return tile.resize((size, size), Image.Resampling.BOX)


def dib(img):
    """32-bit BGRA DIB with an AND mask, as stored in .ico files."""
    w, h = img.size
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes("raw", "BGRA")
    row = ((w + 31) // 32) * 4
    return header + px + b"\0" * (row * h)


def write_ico(path, images):
    blobs = []
    for img in images:
        if img.size[0] >= 256:
            buf = io.BytesIO()
            img.save(buf, "PNG", optimize=True)
            blobs.append(buf.getvalue())
        else:
            blobs.append(dib(img))
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for img, blob in zip(images, blobs):
        w = img.size[0]
        out += struct.pack("<BBBBHHII", w % 256, w % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))


def main():
    polys = subpaths(read_path("AvrPathData"))
    os.makedirs(ASSETS, exist_ok=True)
    for name, muted in (("LegacyAVR.ico", False), ("LegacyAVR-muted.ico", True)):
        write_ico(os.path.join(ASSETS, name), [render(s, polys, muted) for s in SIZES])
        print("wrote", os.path.join(ASSETS, name))


if __name__ == "__main__":
    main()
