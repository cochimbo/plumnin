#!/usr/bin/env python3
# Usage: tools/make_logo.py src/frontend/assets/fonts/chakra_petch_bold_italic.ttf src/frontend/assets
# (needs Pillow). The installer's wizard*.bmp carry the same marquee under the plum.
# Draws plumnin's marquee (logo.png) and the installer's side images, in the style of
# plumsg's: blocky letters with a white outline in a dark box with a glow.
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

FONT = sys.argv[1]
OUT = sys.argv[2]

def marquee(scale=1.0):
    W, H = int(610 * scale), int(230 * scale)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    box = (int(28 * scale), int(48 * scale), W - int(28 * scale), H - int(48 * scale))
    r = int(10 * scale)
    # Glow around the box.
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle(box, r, outline=(255, 90, 90, 255), width=int(10 * scale))
    glow = glow.filter(ImageFilter.GaussianBlur(14 * scale))
    img = Image.alpha_composite(img, glow)
    img = Image.alpha_composite(img, glow)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(box, r, fill=(22, 18, 40, 255), outline=(255, 214, 214, 255), width=max(2, int(4 * scale)))
    inner = (box[0] + int(8 * scale), box[1] + int(8 * scale), box[2] - int(8 * scale), box[3] - int(8 * scale))
    d.rounded_rectangle(inner, max(1, r - 4), outline=(150, 30, 40, 255), width=max(1, int(3 * scale)))

    # The letters: a mask, its outline, and a red gradient with a light top.
    font = ImageFont.truetype(FONT, int(112 * scale))
    text = "PLUMNIN"
    mask = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(mask)
    l, t, rr, b = md.textbbox((0, 0), text, font=font)
    x = (W - (rr - l)) // 2 - l
    y = (H - (b - t)) // 2 - t
    md.text((x, y), text, font=font, fill=255)
    outline = mask.filter(ImageFilter.MaxFilter(2 * int(5 * scale) + 1))
    shadow = outline.filter(ImageFilter.GaussianBlur(3 * scale))
    img.paste((0, 0, 0, 200), (int(4 * scale), int(5 * scale)), shadow)
    img.paste((40, 10, 20, 255), (0, 0), outline.filter(ImageFilter.MaxFilter(3)))
    img.paste((255, 255, 255, 255), (0, 0), outline)
    grad = Image.new("RGBA", (W, H))
    gd = ImageDraw.Draw(grad)
    top, bottom = y + t, y + b
    for yy in range(H):
        k = min(1, max(0, (yy - top) / max(1, bottom - top)))
        if k < 0.45:
            c = (255, int(120 - 60 * k), int(120 - 60 * k))
        else:
            c = (int(235 - 90 * (k - 0.45)), int(40 - 20 * (k - 0.45)), int(52 - 20 * (k - 0.45)))
        gd.line([(0, yy), (W, yy)], fill=c + (255,))
    img.paste(grad, (0, 0), mask)
    # A thin light edge along the top of each letter.
    edge = ImageChops.subtract(mask, ImageChops.offset(mask, 0, int(4 * scale)))
    img.paste((255, 230, 230, 255), (0, 0), edge)
    return img

logo = marquee()
logo.save(OUT + "/logo.png")
