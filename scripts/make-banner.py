#!/usr/bin/env python3
"""The README's banner (docs/images/banner.png, 1600x480): the app icon's wine glass (scripts/make-icon.py) in
front of a program's window, and the title, in Crema's layout.

    python scripts/make-banner.py
"""
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
W, H = 1600, 480
K = 2  # drawn at twice the size and shrunk, for smooth edges
TEXT = (0xF6, 0xEC, 0xF0)
MUTED = (0xC9, 0x96, 0xA8)
GLASS = (236, 230, 240, 255)


def font(names, size):
    for name in names:
        for d in ("C:/Windows/Fonts", "/usr/share/fonts/truetype/dejavu"):
            path = os.path.join(d, name)
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def background():
    """Near black, lit deep red behind the glass"""
    img = Image.new("RGBA", (W, H), (0x10, 0x06, 0x0B, 255))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((130, -60, 770, 560), fill=(0x5C, 0x12, 0x2C, 255))
    return Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(150)))


def window():
    """A program's window behind the glass: its title bar with the minimize, maximize and close glyphs"""
    layer = Image.new("RGBA", (W * K, H * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0, x1, y1 = 262 * K, 74 * K, 638 * K, 338 * K
    bar = (0x47, 0x14, 0x2A, 255)
    d.rounded_rectangle((x0, y0, x1, y1), radius=16 * K, fill=(0x2C, 0x0D, 0x1B, 235))
    d.rounded_rectangle((x0, y0, x1, y0 + 40 * K), radius=16 * K, fill=bar)
    d.rectangle((x0, y0 + 24 * K, x1, y0 + 40 * K), fill=bar)  # the bar's lower corners square
    d.rounded_rectangle((x0, y0, x1, y1), radius=16 * K, outline=(0xE8, 0xC4, 0xD2, 150), width=3 * K)
    c = (0xF0, 0xDA, 0xE2, 255)
    cy = y0 + 20 * K
    r = 7 * K
    xc = x1 - 30 * K  # close
    d.line((xc - r, cy - r, xc + r, cy + r), fill=c, width=3 * K)
    d.line((xc - r, cy + r, xc + r, cy - r), fill=c, width=3 * K)
    xm = x1 - 64 * K  # maximize
    d.rectangle((xm - r, cy - r, xm + r, cy + r), outline=c, width=3 * K)
    xn = x1 - 98 * K  # minimize
    d.line((xn - r, cy + 5 * K, xn + r, cy + 5 * K), fill=c, width=3 * K)
    d.rounded_rectangle((x0 + 18 * K, cy - 5 * K, x0 + 128 * K, cy + 5 * K), radius=5 * K, fill=(0xC9, 0x96, 0xA8, 160))
    return layer.resize((W, H), Image.LANCZOS)


def glass():
    """The icon's glass, larger, standing in front of the window, with a soft glow"""
    s = 0.86 * K
    ox, oy = 450 * K, 416 * K  # where the icon's (256, 392), the foot's bottom, lands

    def p(x, y):
        return (ox + (x - 256) * s, oy + (y - 392) * s)

    def box(x0, y0, x1, y1):
        return (*p(x0, y0), *p(x1, y1))

    layer = Image.new("RGBA", (W * K, H * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    # bowl: glass walls, the air above the wine, the wine (as in scripts/make-icon.py)
    d.polygon([p(166, 84), p(346, 84), p(346, 180), p(166, 180)], fill=GLASS)
    d.chord(box(166, 70, 346, 290), start=0, end=180, fill=GLASS)
    d.polygon([p(174, 84), p(338, 84), p(338, 150), p(174, 150)], fill=(118, 40, 62, 255))
    d.polygon([p(174, 150), p(338, 150), p(338, 180), p(174, 180)], fill=(150, 18, 48, 255))
    d.chord(box(174, 78, 338, 280), start=0, end=180, fill=(150, 18, 48, 255))
    d.line([p(174, 150), p(338, 150)], fill=(196, 52, 84, 255), width=round(4 * s))
    # stem and foot
    d.rectangle(box(248, 286, 264, 372), fill=GLASS)
    d.ellipse(box(186, 360, 326, 392), fill=GLASS)
    layer = layer.resize((W, H), Image.LANCZOS)
    glow = layer.filter(ImageFilter.GaussianBlur(26))
    tint = Image.new("RGBA", (W, H), (0xFF, 0x5A, 0x8A, 0))
    tint.putalpha(glow.getchannel("A").point(lambda a: a * 0.6))
    out = Image.alpha_composite(Image.new("RGBA", (W, H), (0, 0, 0, 0)), tint)
    out = Image.alpha_composite(out, layer)
    # the ground line under it, as in Crema's
    ImageDraw.Draw(out).rounded_rectangle((330, 428, 570, 436), radius=4, fill=(0xB8, 0x3A, 0x62, 200))
    return out


def gradient_text(img, xy_right, y, text, f, left, right):
    """Right-aligned text filled with a left-to-right gradient, like Crema's title"""
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), text, font=f)
    tw = bbox[2] - bbox[0]
    x = xy_right - tw
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).text((x - bbox[0], y - bbox[1]), text, font=f, fill=255)
    grad = Image.new("RGBA", (W, H))
    gd = ImageDraw.Draw(grad)
    for i in range(tw + 1):
        t = i / max(tw, 1)
        c = tuple(round(left[k] + (right[k] - left[k]) * t) for k in range(3)) + (255,)
        gd.line([(x + i, 0), (x + i, H)], fill=c)
    grad.putalpha(ImageChops.multiply(mask, Image.new("L", (W, H), 255)))
    img.alpha_composite(grad)


def right_text(img, x_right, y, text, f, fill):
    d = ImageDraw.Draw(img)
    d.text((x_right - d.textlength(text, font=f), y), text, font=f, fill=fill)


def main():
    img = background()
    img.alpha_composite(window())
    img.alpha_composite(glass())
    bold = font(("segoeuib.ttf", "DejaVuSans-Bold.ttf"), 170)
    light = font(("segoeuil.ttf", "segoeui.ttf", "DejaVuSans.ttf"), 46)
    small = font(("segoeuil.ttf", "segoeui.ttf", "DejaVuSans.ttf"), 36)
    gradient_text(img, 1520, 112, "WinePS4", bold, (0xFF, 0xA8, 0xC2), (0xE0, 0x30, 0x62))
    right_text(img, 1520, 318, "WINDOWS PROGRAMS ON THE PS4", light, TEXT)
    right_text(img, 1520, 380, "WINE 11  ·  NATIVE APP  ·  DXVK", small, MUTED)
    out = os.path.join(ROOT, "docs", "images")
    os.makedirs(out, exist_ok=True)
    img.convert("RGB").save(os.path.join(out, "banner.png"))
    print("wrote docs/images/banner.png")


if __name__ == "__main__":
    main()
