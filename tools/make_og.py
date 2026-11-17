#!/usr/bin/env python3
"""Share image for loudestnoi.se/esp-sim: a cartoon ESP32-S3 beaming to a pod and a CGM.

Usage: python3 tools/make_og.py flasher/og.png   (needs Pillow, and the Lato and
Noto Sans Mono fonts at the Debian/Ubuntu paths below; adjust LATO/MONO elsewhere).
"""
import math
import random
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H, S = 1200, 630, 3
LATO = "/usr/share/fonts/truetype/lato/"
MONO = "/usr/share/fonts/truetype/noto/NotoSansMono-Bold.ttf"
OUT = sys.argv[1] if len(sys.argv) > 1 else "og.png"

BG1, BG2 = (14, 18, 30), (24, 31, 52)
BLUE, MINT, ORANGE, PINK = (79, 156, 248), (61, 220, 151), (255, 159, 67), (255, 107, 157)
INK, MUTED = (236, 240, 246), (150, 162, 182)


def s(v):
    return int(v * S)


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def font(name, size):
    return ImageFont.truetype(name if name.startswith("/") else LATO + name, s(size))


img = Image.new("RGB", (s(W), s(H)))
d = ImageDraw.Draw(img)
for y in range(s(H)):
    d.line([(0, y), (s(W), y)], fill=mix(BG1, BG2, y / s(H)))

# Dot grid like a breadboard.
layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
g = ImageDraw.Draw(layer)
for x in range(20, W, 30):
    for y in range(20, H, 30):
        g.ellipse([s(x - 1.4), s(y - 1.4), s(x + 1.4), s(y + 1.4)], fill=(255, 255, 255, 22))

# A few circuit traces wandering in from the edges.
random.seed(7)
for _ in range(9):
    x, y = random.choice([(random.randrange(560, W, 30), random.choice([-10, H + 10])),
                          (W + 10, random.randrange(20, H, 30))])
    pts = [(x, y)]
    for _ in range(4):
        dx = random.choice([-1, 0, 1]) * random.randrange(30, 120, 30)
        dy = random.choice([-1, 1]) * random.randrange(30, 120, 30)
        x, y = x + dx, y + dy
        pts.append((x, y))
    g.line([(s(a), s(b)) for a, b in pts], fill=BLUE + (40,), width=s(2), joint="curve")
    g.ellipse([s(x - 4), s(y - 4), s(x + 4), s(y + 4)], outline=BLUE + (70,), width=s(2))
img.paste(layer, (0, 0), layer)

# Soft glow behind the board.
glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse([s(720), s(170), s(1060), s(500)], fill=BLUE + (70,))
glow = glow.filter(ImageFilter.GaussianBlur(s(60)))
img.paste(glow, (0, 0), glow)

art = Image.new("RGBA", img.size, (0, 0, 0, 0))
a = ImageDraw.Draw(art)

# --- The board (tilted a little for fun) ---
board = Image.new("RGBA", (s(220), s(330)), (0, 0, 0, 0))
b = ImageDraw.Draw(board)
b.rounded_rectangle([0, 0, s(220), s(330)], radius=s(14), fill=(22, 110, 72))
b.rounded_rectangle([s(6), s(6), s(214), s(324)], radius=s(10), outline=(40, 150, 100), width=s(2))
for i in range(15):  # header pins down both sides
    py = s(36 + i * 18)
    for px in (s(14), s(196)):
        b.rectangle([px, py, px + s(10), py + s(10)], fill=(214, 180, 90))
        b.ellipse([px + s(3), py + s(3), px + s(7), py + s(7)], fill=(30, 30, 30))
# Module can with antenna zigzag on top.
b.rounded_rectangle([s(42), s(30), s(178), s(190)], radius=s(6), fill=(196, 202, 210))
b.rounded_rectangle([s(50), s(38), s(170), s(182)], radius=s(4), fill=(176, 183, 193))
b.text((s(110), s(148)), "ESP32-S3", font=font(MONO, 17), fill=(70, 76, 88), anchor="mm")
b.text((s(110), s(168)), "WROOM-1", font=font(MONO, 12), fill=(100, 106, 118), anchor="mm")
zig = [(s(56 + i * 12), s(14 if i % 2 else 26)) for i in range(10)]
b.line(zig, fill=(214, 180, 90), width=s(4), joint="curve")
# USB-C and two buttons.
b.rounded_rectangle([s(80), s(300), s(140), s(330)], radius=s(6), fill=(150, 156, 166))
b.rounded_rectangle([s(88), s(308), s(132), s(322)], radius=s(4), fill=(40, 44, 52))
for bx, lab in ((s(52), "BOOT"), (s(148), "RST")):
    b.rounded_rectangle([bx, s(236), bx + s(26), s(262)], radius=s(4), fill=(60, 64, 72))
    b.ellipse([bx + s(6), s(242), bx + s(20), s(256)], fill=(30, 32, 36))
    b.text((bx + s(13), s(276)), lab, font=font(MONO, 10), fill=(200, 230, 210), anchor="mm")
# Face on the module can.
for ex in (82, 138):
    b.ellipse([s(ex - 8), s(66), s(ex + 8), s(82)], fill=(30, 34, 42))
    b.ellipse([s(ex - 3), s(68), s(ex + 1), s(72)], fill=(255, 255, 255))
b.arc([s(88), s(78), s(132), s(112)], 20, 160, fill=(30, 34, 42), width=s(4))
# Status LED.
b.ellipse([s(100), s(212), s(120), s(232)], fill=MINT)
board = board.rotate(-8, resample=Image.BICUBIC, expand=True)
bx, by = s(780), s(150)
art.paste(board, (bx, by), board)

# --- Radio waves out to each "device" ---
def waves(ox, oy, ang, color):
    for i, r in enumerate((40, 64, 88)):
        a.arc([s(ox - r), s(oy - r), s(ox + r), s(oy + r)], ang - 28, ang + 28,
              fill=color + (230 - i * 60,), width=s(5))

waves(990, 180, -40, ORANGE)
waves(1010, 410, 35, PINK)
waves(770, 175, -150, MINT)

# Pod: a rounded capsule, sim-orange.
pod = Image.new("RGBA", (s(170), s(110)), (0, 0, 0, 0))
p = ImageDraw.Draw(pod)
p.rounded_rectangle([0, 0, s(170), s(110)], radius=s(48), fill=(248, 248, 250))
p.rounded_rectangle([s(10), s(10), s(160), s(100)], radius=s(40), outline=ORANGE, width=s(5))
p.text((s(85), s(56)), "POD", font=font("Lato-Black.ttf", 30), fill=ORANGE, anchor="mm")
pod = pod.rotate(14, resample=Image.BICUBIC, expand=True)
art.paste(pod, (s(1000), s(40)), pod)

# CGM: a little disc with a readout bubble.
a.ellipse([s(1052), s(430), s(1152), s(530)], fill=(248, 248, 250))
a.ellipse([s(1066), s(444), s(1138), s(516)], outline=PINK, width=s(5))
a.text((s(1102), s(481)), "G7", font=font("Lato-Black.ttf", 28), fill=PINK, anchor="mm")
a.rounded_rectangle([s(940), s(540), s(1090), s(592)], radius=s(26), fill=PINK)
a.text((s(1015), s(567)), "112 →", font=font("Lato-Black.ttf", 28), fill=(30, 20, 30), anchor="mm")

# Phone peeking in at upper-left of the board with a test badge.
a.rounded_rectangle([s(600), s(70), s(690), s(240)], radius=s(16), fill=(40, 46, 62), outline=(90, 100, 124), width=s(3))
a.rounded_rectangle([s(608), s(84), s(682), s(226)], radius=s(10), fill=(18, 22, 34))
a.rounded_rectangle([s(616), s(100), s(674), s(124)], radius=s(12), fill=MINT)
a.text((s(645), s(112)), "TEST", font=font(MONO, 13), fill=(10, 30, 20), anchor="mm")
a.line([(s(620), s(170)), (s(636), s(150)), (s(650), s(162)), (s(670), s(140))], fill=BLUE, width=s(4), joint="curve")
a.text((s(645), s(204)), "Loop / Trio", font=font("Lato-Bold.ttf", 11), fill=MUTED, anchor="mm")

img.paste(art, (0, 0), art)

# --- Text ---
t = ImageDraw.Draw(img)
t.rounded_rectangle([s(64), s(78), s(300), s(112)], radius=s(17), fill=(32, 42, 66))
t.text((s(182), s(95)), "one board \u00b7 two devices", font=font("Lato-Bold.ttf", 17), fill=MINT, anchor="mm")
t.text((s(60), s(140)), "ESP-Sim", font=font("Lato-Black.ttf", 112), fill=INK)
t.text((s(64), s(282)), "A fake insulin pod and a fake", font=font("Lato-Semibold.ttf", 34), fill=INK)
t.text((s(64), s(324)), "G7 sensor, on one ESP32-S3.", font=font("Lato-Semibold.ttf", 34), fill=INK)
t.text((s(64), s(386)), "For testing Loop & Trio on a test phone.", font=font("Lato-Regular.ttf", 24), fill=MUTED)
t.text((s(64), s(418)), "No insulin. Made-up numbers. Big fun.", font=font("Lato-Regular.ttf", 24), fill=MUTED)

# Fake terminal prompt.
t.rounded_rectangle([s(60), s(486), s(520), s(548)], radius=s(12), fill=(8, 11, 20), outline=(48, 58, 84), width=s(2))
t.text((s(84), s(517)), "$ flash it from your browser", font=font(MONO, 22), fill=MINT, anchor="lm")
t.rectangle([s(470), s(504), s(482), s(530)], fill=MINT)
t.text((s(64), s(578)), "loudestnoi.se/esp-sim", font=font("Lato-Bold.ttf", 22), fill=BLUE, anchor="lm")

img.resize((W, H), Image.LANCZOS).save(OUT, optimize=True)
print(OUT)
