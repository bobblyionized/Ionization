"""Generate assets/app.ico (and a 256px PNG) for Ionization without external deps.

Mark: dark rounded tile, three motion streaks trailing into a volt orb.
"""
import math, os, struct, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets")

def hexc(h):
    return tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5))

BG, EDGE, FG, VOLT = hexc("#0d0e11"), hexc("#2b2e35"), hexc("#f3f4f6"), hexc("#d4f53c")

def sd_round_box(px, py, cx, cy, hw, hh, r):
    qx, qy = abs(px - cx) - hw + r, abs(py - cy) - hh + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r

def sd_capsule(px, py, ax, ay, bx, by, r):
    pax, pay, bax, bay = px - ax, py - ay, bx - ax, by - ay
    h = max(0, min(1, (pax * bax + pay * bay) / (bax * bax + bay * bay)))
    return math.hypot(pax - bax * h, pay - bay * h) - r

def over(dst, col, a):
    return tuple(d * (1 - a) + c * a for d, c in zip(dst[:3], col)) + (dst[3] + a * (1 - dst[3]),)

def shade(x, y, px):
    """x, y in 0..1. Returns premultiplied-free RGBA floats."""
    aa = 1.0 / px
    c = (0.0, 0.0, 0.0, 0.0)
    tile = sd_round_box(x, y, 0.5, 0.5, 0.46, 0.46, 0.13)
    a = max(0, min(1, 0.5 - tile / aa))
    if a <= 0:
        return c
    c = (BG[0], BG[1], BG[2], a)
    # Inner edge line + top sheen.
    edge = abs(tile + aa * 0.8) - aa * 0.6
    c = over(c, EDGE, max(0, min(1, 0.5 - edge / aa)) * a)
    if y < 0.5:
        c = over(c, FG, 0.035 * (0.5 - y) * 2 * a)
    # Motion streaks.
    streaks = [(0.36, 0.20, 0.50, 0.32), (0.50, 0.12, 0.44, 0.75), (0.64, 0.20, 0.50, 0.32)]
    for sy, x0, x1, alpha in streaks:
        d = sd_capsule(x, y, x0, sy, x1, sy, 0.034)
        k = max(0, min(1, 0.5 - d / aa))
        fade = 0.35 + 0.65 * max(0, min(1, (x - x0) / max(1e-6, x1 - x0)))
        c = over(c, FG, k * alpha * fade * a)
    # Glow + orb.
    d = math.hypot(x - 0.64, y - 0.5)
    glow = max(0, 1 - d / 0.33) ** 2 * 0.28
    c = over(c, VOLT, glow * a)
    orb = d - 0.165
    c = over(c, VOLT, max(0, min(1, 0.5 - orb / aa)) * a)
    hi = math.hypot(x - 0.605, y - 0.455) - 0.05
    c = over(c, (1, 1, 1), max(0, min(1, 0.5 - hi / aa)) * 0.35 * max(0, min(1, 0.5 - orb / aa)))
    return c

def render(size):
    ss = 3 if size <= 64 else 2
    rows = []
    for j in range(size):
        row = bytearray()
        for i in range(size):
            r = g = b = al = 0.0
            for sj in range(ss):
                for si in range(ss):
                    x = (i + (si + 0.5) / ss) / size
                    y = (j + (sj + 0.5) / ss) / size
                    cr, cg, cb, ca = shade(x, y, size * ss)
                    r += cr * ca; g += cg * ca; b += cb * ca; al += ca
            n = ss * ss
            al /= n
            if al > 0:
                r, g, b = r / n / al, g / n / al, b / n / al
            row += bytes((int(max(0, min(1, r)) * 255 + 0.5), int(max(0, min(1, g)) * 255 + 0.5),
                          int(max(0, min(1, b)) * 255 + 0.5), int(al * 255 + 0.5)))
        rows.append(bytes(row))
    return rows

def png(rows, size):
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def main():
    os.makedirs(OUT, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [(s, png(render(s), s)) for s in sizes]
    with open(os.path.join(OUT, "app.png"), "wb") as f:
        f.write(images[-1][1])
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for s, data in images:
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    with open(os.path.join(OUT, "app.ico"), "wb") as f:
        f.write(header + entries + blobs)
    print("wrote", os.path.join(OUT, "app.ico"))

if __name__ == "__main__":
    main()
