#!/usr/bin/env python3
"""Draws the launcher icon of the Android port (original artwork in the spirit of Prototype's interface: red neon
brackets and claw slashes on black) and writes android/res/mipmap-*/ic_launcher.png.

Usage: tools/make_icon.py"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
RED = (224, 38, 30)


def draw(n=1024, layers=False):
    img = Image.new("RGB", (n, n), (8, 6, 6))
    # radial red haze
    haze = Image.new("L", (n, n), 0)
    hd = ImageDraw.Draw(haze)
    for r in range(n // 2, 0, -8):
        hd.ellipse((n / 2 - r, n / 2 - r, n / 2 + r, n / 2 + r), fill=int(90 * (1 - r / (n / 2)) ** 1.6))
    img.paste((120, 12, 8), (0, 0), haze.filter(ImageFilter.GaussianBlur(40)))

    # scanlines
    sd = ImageDraw.Draw(img)
    for y in range(0, n, 12):
        sd.line((0, y, n, y), fill=(0, 0, 0), width=3)

    layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    w = n // 22
    m, arm = n * 0.17, n * 0.16
    # bracket corners [ ]
    for x0, sx in ((m, 1), (n - m, -1)):
        d.line((x0, m, x0, n - m), fill=RED + (255,), width=w)
        d.line((x0 - sx * w / 2, m, x0 + sx * arm, m), fill=RED + (255,), width=w)
        d.line((x0 - sx * w / 2, n - m, x0 + sx * arm, n - m), fill=RED + (255,), width=w)
    # three claw slashes, tapered
    for k in range(3):
        cx = n * (0.38 + 0.12 * k)
        top, bottom = n * (0.28 + 0.02 * k), n * (0.74 - 0.02 * k)
        ang = math.radians(18)
        x0, y0 = cx - (bottom - top) / 2 * math.tan(ang), top
        x1, y1 = cx + (bottom - top) / 2 * math.tan(ang), bottom
        steps = 40
        for i in range(steps):
            t0, t1 = i / steps, (i + 1) / steps
            width = int(n * 0.045 * math.sin(math.pi * (t0 + t1) / 2) + 2)
            d.line((x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0, x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1),
                   fill=(255, 236, 230, 255), width=width)
    glow = layer.filter(ImageFilter.GaussianBlur(n / 40))
    tint = Image.new("RGBA", (n, n), RED + (0,))
    tint.putalpha(glow.getchannel("A"))
    if layers:  # adaptive icon: background and foreground separately
        fg = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        fg.alpha_composite(tint)
        fg.alpha_composite(tint)
        fg.alpha_composite(layer)
        return img, fg
    img.paste(tint, (0, 0), tint)
    img.paste(tint, (0, 0), tint)
    img.paste(layer, (0, 0), layer)
    return img


def main():
    big = draw()
    for name, px in SIZES.items():
        out = os.path.join(ROOT, "android", "res", f"mipmap-{name}", "ic_launcher.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        big.resize((px, px), Image.LANCZOS).save(out)
    big.resize((512, 512), Image.LANCZOS).save(os.path.join(ROOT, "android", "icon-512.png"))
    # Adaptive icon (API 26+): 108 dp layers, artwork inside the 66 dp safe zone.
    bg, fg = draw(layers=True)
    n = fg.size[0]
    safe = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    inner = int(n * 0.70)
    safe.alpha_composite(fg.resize((inner, inner), Image.LANCZOS), ((n - inner) // 2, (n - inner) // 2))
    for name, px in SIZES.items():
        layer_px = px * 108 // 48
        d = os.path.join(ROOT, "android", "res", f"mipmap-{name}")
        bg.resize((layer_px, layer_px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_bg.png"))
        safe.resize((layer_px, layer_px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_fg.png"))
    xml = os.path.join(ROOT, "android", "res", "mipmap-anydpi-v26", "ic_launcher.xml")
    os.makedirs(os.path.dirname(xml), exist_ok=True)
    with open(xml, "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@mipmap/ic_launcher_bg" />\n'
                '    <foreground android:drawable="@mipmap/ic_launcher_fg" />\n'
                '</adaptive-icon>\n')
    print("icons written")


if __name__ == "__main__":
    main()
