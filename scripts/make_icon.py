# make_icon.py: the loader's icon - loader/dittosr.ico (16 to 256 px), loader/dittosr-256.png - drawn as
# loader/dittosr.svg describes (512 units): a light pink rounded square, a white face of two dot eyes and a smile.
# Needs Pillow. Run from anywhere: python scripts/make_icon.py
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
LOADER = os.path.join(HERE, "..", "loader")
TOP, BOTTOM = (0xF6, 0xBF, 0xE6), (0xE8, 0x9C, 0xD6)  # (dittosr.svg's gradient)
S = 2048  # (drawn at 4 x the svg's 512 units, then reduced: smooth edges)
k = S / 512


def master():
    grad = Image.new("RGB", (1, S))
    for y in range(S):
        t = y / (S - 1)
        grad.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)))
    grad = grad.resize((S, S))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=round(112 * k), fill=255)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    for cx in (190, 322):  # the eyes
        r = 30
        d.ellipse(((cx - r) * k, (214 - r) * k, (cx + r) * k, (214 + r) * k), fill=white)
    # the smile: a shallow arc, round-capped (dittosr.svg: M150 300 Q256 388 362 300) - a disc swept along it
    r = 34 / 2 * k
    for i in range(2001):
        t = i / 2000
        x = ((1 - t) ** 2 * 150 + 2 * (1 - t) * t * 256 + t * t * 362) * k
        y = ((1 - t) ** 2 * 300 + 2 * (1 - t) * t * 388 + t * t * 300) * k
        d.ellipse((x - r, y - r, x + r, y + r), fill=white)
    return img


m = master()
m.resize((256, 256), Image.LANCZOS).save(os.path.join(LOADER, "dittosr-256.png"))
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
frames = [m.resize((n, n), Image.LANCZOS) for n in sizes]
frames[-1].save(os.path.join(LOADER, "dittosr.ico"), sizes=[(n, n) for n in sizes], append_images=frames[:-1])
print("wrote loader/dittosr.ico, loader/dittosr-256.png")
