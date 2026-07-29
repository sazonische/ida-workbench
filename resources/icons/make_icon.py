"""Render the app icons and checkbox check marks.

Outputs (next to this file):
  app.png        256, white squircle background   (source for the .ico)
  app.ico        16..256, for the exe file icon
  app_nobg.png   256, transparent background       (in-app window/taskbar icon)
  check_light.png  18px filled checkbox tick
"""
import os
import math
from PIL import Image, ImageDraw, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024

WHITE = (255, 255, 255, 255)
BG    = (252, 252, 253, 255)
BLUE  = (66, 133, 244, 255)
GREEN = (52, 168, 83, 255)
AMBER = (245, 166, 35, 255)
DARK  = (60, 64, 67, 255)

x0, bar_h = 150, 148
BARS = [(BLUE, 205, 660), (GREEN, 385, 620), (AMBER, 565, 620)]
C, R_out, RING_W, GAP = (688, 566), 152, 46, 30


def draw_bars(d):
    for color, y, x1 in BARS:
        cy = y + bar_h // 2
        d.rounded_rectangle([x0, y, x1, y + bar_h], radius=44, fill=color)
        d.ellipse([x0 + 44, cy - 27, x0 + 44 + 54, cy + 27], fill=WHITE)
        d.rounded_rectangle([x0 + 150, cy - 18, x0 + 150 + 250, cy + 18], radius=18, fill=WHITE)


def draw_magnifier(d):
    ang = math.radians(45)
    h0 = (C[0] + math.cos(ang) * (R_out - 6), C[1] + math.sin(ang) * (R_out - 6))
    h1 = (C[0] + math.cos(ang) * (R_out + 150), C[1] + math.sin(ang) * (R_out + 150))
    hw = 30
    d.line([h0, h1], fill=DARK, width=hw * 2)
    for pt in (h0, h1):
        d.ellipse([pt[0] - hw, pt[1] - hw, pt[0] + hw, pt[1] + hw], fill=DARK)
    d.ellipse([C[0] - R_out, C[1] - R_out, C[0] + R_out, C[1] + R_out], outline=DARK, width=RING_W)


def gap_box(extra=0):
    r = R_out + GAP + extra
    return [C[0] - r, C[1] - r, C[0] + r, C[1] + r]


# --- full icon (with squircle background) ---
full = Image.new("RGBA", (S, S), (0, 0, 0, 0))
fd = ImageDraw.Draw(full)
fd.rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.225), fill=BG)
draw_bars(fd)
fd.ellipse(gap_box(), fill=BG)          # white gap blends into the white bg
draw_magnifier(fd)
full.resize((256, 256), Image.LANCZOS).save(os.path.join(HERE, "app.png"))
full.resize((256, 256), Image.LANCZOS).save(
    os.path.join(HERE, "app.ico"),
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

# --- background-less icon (transparent gap punched through the bars) ---
nobg = Image.new("RGBA", (S, S), (0, 0, 0, 0))
nd = ImageDraw.Draw(nobg)
draw_bars(nd)
mask = Image.new("L", (S, S), 255)      # 0 = erase to transparent
ImageDraw.Draw(mask).ellipse(gap_box(), fill=0)
r, g, b, a = nobg.split()
nobg = Image.merge("RGBA", (r, g, b, ImageChops.multiply(a, mask)))
draw_magnifier(ImageDraw.Draw(nobg))    # ring + handle over the transparent gap
bbox = nobg.getbbox()
cropped = nobg.crop(bbox)
side = max(cropped.size)
pad = int(side * 0.06)
canvas = Image.new("RGBA", (side + 2 * pad, side + 2 * pad), (0, 0, 0, 0))
canvas.paste(cropped, ((canvas.width - cropped.width) // 2, (canvas.height - cropped.height) // 2))
canvas.resize((256, 256), Image.LANCZOS).save(os.path.join(HERE, "app_nobg.png"))


# --- checkbox tick (filled rounded square + tick), one per theme ---
def make_check(path, fill, tick):
    s = 72
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    dd = ImageDraw.Draw(im)
    dd.rounded_rectangle([0, 0, s - 1, s - 1], radius=20, fill=fill)
    dd.line([(0.24 * s, 0.52 * s), (0.42 * s, 0.70 * s), (0.76 * s, 0.30 * s)],
            fill=tick, width=9, joint="curve")
    im.resize((18, 18), Image.LANCZOS).save(path)


make_check(os.path.join(HERE, "check_light.png"), (101, 85, 143, 255), WHITE)
print("wrote app.png, app.ico, app_nobg.png, check_light.png")
