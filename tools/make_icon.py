#!/usr/bin/env python3
"""Génère l'icône de NXGo (icon.jpg, 256x256) : un coin de goban avec deux pierres.

  python tools/make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

SIZE = 256
SS = 4  # suréchantillonnage pour l'anticrénelage
OUT = Path(__file__).resolve().parent.parent / "icon.jpg"


def stone(img, cx, cy, r, black):
    """Pierre ombrée : dégradé radial décalé vers le reflet."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    px = layer.load()
    hx, hy = cx - 0.35 * r, cy - 0.4 * r
    for y in range(int(cy - r) - 1, int(cy + r) + 2):
        for x in range(int(cx - r) - 1, int(cx + r) + 2):
            if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                continue
            d = min(1.0, ((x - hx) ** 2 + (y - hy) ** 2) ** 0.5 / (1.6 * r))
            if black:
                t = d ** 0.6
                c = tuple(int(a + (b - a) * t) for a, b in zip((110, 110, 116), (14, 14, 18)))
            else:
                t = d ** 1.6
                c = tuple(int(a + (b - a) * t) for a, b in zip((255, 255, 252), (186, 184, 178)))
            px[x, y] = c + (255,)
    img.alpha_composite(layer)


def main():
    s = SIZE * SS
    img = Image.new("RGBA", (s, s), (226, 186, 116, 255))
    d = ImageDraw.Draw(img)
    # Veinage léger
    for i in range(0, s, 18 * SS // 4):
        d.line([(0, i), (s, i + 40 * SS)], fill=(214, 172, 102, 255), width=3 * SS)
    cell = s / 4.6
    origin = cell * 0.8
    for i in range(5):
        p = origin + i * cell
        d.line([(p, origin), (p, s)], fill=(40, 28, 14, 255), width=int(2.2 * SS))
        d.line([(origin, p), (s, p)], fill=(40, 28, 14, 255), width=int(2.2 * SS))
    d.line([(origin, origin), (s, origin)], fill=(40, 28, 14, 255), width=int(4 * SS))
    d.line([(origin, origin), (origin, s)], fill=(40, 28, 14, 255), width=int(4 * SS))
    r = cell * 0.48
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    for (gx, gy) in ((2, 2), (3, 1)):
        cx, cy = origin + gx * cell + 4 * SS, origin + gy * cell + 6 * SS
        sd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(0, 0, 0, 90))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(4 * SS)))
    stone(img, origin + 2 * cell, origin + 2 * cell, r, True)
    stone(img, origin + 3 * cell, origin + 1 * cell, r, False)
    img.convert("RGB").resize((SIZE, SIZE), Image.LANCZOS).save(OUT, quality=92)
    print(f"✓ {OUT}")


if __name__ == "__main__":
    main()
