"""Draws app/sce_sys/icon0.png (512x512): a wine glass on a deep red field, "WinePS4" under it."""
import pathlib
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE = 512
out = pathlib.Path(__file__).resolve().parent.parent / "app" / "sce_sys" / "icon0.png"
out.parent.mkdir(parents=True, exist_ok=True)

img = Image.new("RGB", (SIZE, SIZE))
d = ImageDraw.Draw(img)
for y in range(SIZE):  # vertical gradient
    t = y / SIZE
    d.line([(0, y), (SIZE, y)], fill=(int(70 + 40 * (1 - t)), int(8 + 6 * (1 - t)), int(24 + 18 * (1 - t))))

glow = Image.new("L", (SIZE, SIZE), 0)
ImageDraw.Draw(glow).ellipse((96, 40, 416, 360), fill=90)
img.paste((200, 60, 90), (0, 0), glow.filter(ImageFilter.GaussianBlur(60)))

d = ImageDraw.Draw(img)
glass = (236, 230, 240)
# bowl: glass walls, the air above the wine, the wine
d.polygon([(166, 84), (346, 84), (346, 180), (166, 180)], fill=glass)
d.chord((166, 70, 346, 290), start=0, end=180, fill=glass)
d.polygon([(174, 84), (338, 84), (338, 150), (174, 150)], fill=(118, 40, 62))
d.polygon([(174, 150), (338, 150), (338, 180), (174, 180)], fill=(150, 18, 48))
d.chord((174, 78, 338, 280), start=0, end=180, fill=(150, 18, 48))
d.line([(174, 150), (338, 150)], fill=(196, 52, 84), width=4)
# stem and foot
d.rectangle((248, 286, 264, 372), fill=glass)
d.ellipse((186, 360, 326, 392), fill=glass)

try:
    font = ImageFont.truetype("arialbd.ttf", 66)
except OSError:
    font = ImageFont.load_default()
text = "WinePS4"
w = d.textlength(text, font=font)
d.text(((SIZE - w) / 2, 412), text, font=font, fill=(250, 244, 246))
img.save(out)
print(out)
