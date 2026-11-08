"""Generate Magi's placeholder icons and caller portraits (original artwork).

    python tools/make_assets.py

Writes PNGs into static/codec/icons/ and static/codec/portraits/. Needs Pillow.
Re-running overwrites them, so keep your own portraits under different names
or don't re-run after swapping them in.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent / "static" / "codec"
G = (125, 255, 154)
G_DIM = (30, 90, 48)
BG = (0, 8, 3)


def scanlines(img, step=4, alpha=70):
    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    for y in range(0, img.size[1], step):
        d.line([(0, y), (img.size[0], y)], fill=(0, 0, 0, alpha))
    return Image.alpha_composite(img.convert("RGBA"), over)


def glow(img, radius=6):
    return Image.alpha_composite(img.filter(ImageFilter.GaussianBlur(radius)), img)


# ------------------------------------------------------------------ icons
def icon(size, maskable=False):
    img = Image.new("RGBA", (size, size), BG + (255,))
    d = ImageDraw.Draw(img)
    s = size / 512
    pad = 110 if maskable else 40  # maskable: keep art inside the safe zone
    box = [pad * s, pad * s, size - pad * s, size - pad * s]
    if not maskable:
        d.rounded_rectangle([8 * s, 8 * s, size - 8 * s, size - 8 * s], radius=90 * s, outline=G_DIM, width=int(10 * s))
    # signal arcs around a dot, like a radio
    cx, cy = size / 2, size / 2 + 40 * s * (0.6 if maskable else 1)
    span = (box[2] - box[0]) / 2
    for i, r in enumerate((0.35, 0.62, 0.9)):
        rr = span * r
        d.arc([cx - rr, cy - rr, cx + rr, cy + rr], 225, 315, fill=G, width=max(2, int((26 - i * 3) * s)))
    dot = 26 * s
    d.ellipse([cx - dot, cy - dot, cx + dot, cy + dot], fill=G)
    # "M" bar at the bottom
    y = cy + span * 0.32
    d.text((cx, y), "MAGI", fill=G, anchor="mt", font_size=int(64 * s * (0.8 if maskable else 1)))
    return glow(img, 4 * s).convert("RGB")


def badge(size=96):
    """Android status-bar badge: white on transparent, simple shape."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size / 2, size * 0.62
    for r, w in ((0.18, 8), (0.32, 8), (0.46, 8)):
        rr = size * r
        d.arc([cx - rr, cy - rr, cx + rr, cy + rr], 225, 315, fill="white", width=w)
    d.ellipse([cx - 7, cy - 7, cx + 7, cy + 7], fill="white")
    return img


# -------------------------------------------------------------- portraits
W, H = 360, 480


def bust(d, color, neck=True):
    # shoulders
    d.chord([20, 330, 340, 640], 180, 360, fill=color)
    if neck:
        d.rectangle([140, 270, 220, 360], fill=color)
    # head
    d.ellipse([95, 70, 265, 300], fill=color)


def portrait_stereo_ops():
    img = Image.new("RGBA", (W, H), BG + (255,))
    d = ImageDraw.Draw(img)
    bust(d, (22, 70, 38))
    # headset band + ear cup
    d.arc([80, 55, 280, 260], 200, 340, fill=G_DIM, width=12)
    d.rounded_rectangle([70, 165, 100, 225], radius=10, fill=G_DIM)
    d.line([(90, 220), (150, 265)], fill=G_DIM, width=6)
    d.ellipse([140, 255, 162, 275], fill=G)
    # stereo goggles: two lenses
    for x in (128, 190):
        d.ellipse([x, 150, x + 46, 196], fill=(0, 20, 8), outline=G, width=5)
        d.ellipse([x + 12, 162, x + 24, 174], fill=G)
    d.line([(174, 173), (190, 173)], fill=G, width=5)
    return img


def portrait_disk_warden():
    img = Image.new("RGBA", (W, H), BG + (255,))
    d = ImageDraw.Draw(img)
    bust(d, (22, 70, 38))
    # helmet with brim
    d.chord([85, 55, 275, 250], 180, 360, fill=G_DIM)
    d.rectangle([75, 148, 285, 162], fill=G_DIM)
    # visor slit
    d.rounded_rectangle([120, 185, 240, 205], radius=8, fill=(0, 20, 8), outline=G, width=3)
    # disk emblem on chest
    cx, cy = 180, 410
    d.ellipse([cx - 42, cy - 42, cx + 42, cy + 42], outline=G, width=6)
    d.ellipse([cx - 10, cy - 10, cx + 10, cy + 10], fill=G)
    d.line([(cx + 12, cy - 4), (cx + 34, cy - 22)], fill=G, width=5)
    return img


def portrait_sentry():
    img = Image.new("RGBA", (W, H), BG + (255,))
    d = ImageDraw.Draw(img)
    bust(d, (22, 70, 38))
    # hood
    d.chord([70, 50, 290, 330], 160, 380, fill=G_DIM)
    d.ellipse([110, 110, 250, 300], fill=(22, 70, 38))
    # single camera-lens eye
    cx, cy = 180, 185
    d.ellipse([cx - 34, cy - 34, cx + 34, cy + 34], fill=(0, 20, 8), outline=G, width=6)
    d.ellipse([cx - 14, cy - 14, cx + 14, cy + 14], fill=G)
    d.ellipse([cx - 4, cy - 10, cx + 4, cy - 2], fill=(0, 20, 8))
    # antenna
    d.line([(250, 110), (290, 30)], fill=G_DIM, width=6)
    d.ellipse([282, 20, 298, 36], fill=G)
    return img


def finish_portrait(img):
    return scanlines(glow(img, 3)).convert("RGB")


def main():
    (ROOT / "icons").mkdir(parents=True, exist_ok=True)
    (ROOT / "portraits").mkdir(parents=True, exist_ok=True)
    icon(192).save(ROOT / "icons" / "icon-192.png")
    icon(512).save(ROOT / "icons" / "icon-512.png")
    icon(512, maskable=True).save(ROOT / "icons" / "icon-maskable-512.png")
    badge().save(ROOT / "icons" / "badge-96.png")
    finish_portrait(portrait_stereo_ops()).save(ROOT / "portraits" / "stereo-ops.png")
    finish_portrait(portrait_disk_warden()).save(ROOT / "portraits" / "disk-warden.png")
    finish_portrait(portrait_sentry()).save(ROOT / "portraits" / "sentry.png")
    print("assets written to", ROOT)


if __name__ == "__main__":
    main()
