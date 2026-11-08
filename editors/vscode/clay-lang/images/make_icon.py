"""Draws the Clay extension icon (images/icon.png, 128x128).

Run: python3 images/make_icon.py   (needs Pillow). Drawn at 1024 px and
scaled down, so the edges are smooth at 128.
"""
from PIL import Image, ImageDraw
import math, os

S = 1024
TOP, BOTTOM = (214, 112, 68), (163, 72, 40)    # clay: light to dark terracotta
WHITE, CREAM = (255, 255, 255), (255, 228, 196)

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# rounded square with a vertical gradient
grad = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / (S - 1)
    gd.line([(0, y), (S, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)) + (255,))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=230, fill=255)
img.paste(grad, (0, 0), mask)

d = ImageDraw.Draw(img)
cx, cy, r, w = 470, 512, 300, 130            # the C: an arc opening to the right
start, end = 40, 320                        # degrees (0 = east, clockwise)
d.arc([cx - r, cy - r, cx + r, cy + r], start=start, end=end, fill=WHITE, width=w)
for a in (start, end):                      # round caps
    rad = math.radians(a)
    mx, my = cx + (r - w / 2) * math.cos(rad), cy + (r - w / 2) * math.sin(rad)
    d.ellipse([mx - w / 2, my - w / 2, mx + w / 2, my + w / 2], fill=WHITE)

dot_r = 78                                  # the dot in the C's opening
dx = cx + r - 10
d.ellipse([dx - dot_r, cy - dot_r, dx + dot_r, cy + dot_r], fill=CREAM)

here = os.path.dirname(os.path.abspath(__file__))
img.resize((128, 128), Image.LANCZOS).save(os.path.join(here, "icon.png"))
img.resize((512, 512), Image.LANCZOS).save(os.path.join(here, "icon-512.png"))
print("wrote icon.png (128x128) and icon-512.png")
