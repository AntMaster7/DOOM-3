"""triptych.py a.tga b.tga out.png [scale]: a | b | 8x difference side by side, for a quick look"""
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert('RGB'); b = Image.open(sys.argv[2]).convert('RGB')
scale = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0 / 3.0
d = ImageChops.difference(a, b).point(lambda v: min(255, v * 8))
w, h = a.size
tw, th = int(w * scale), int(h * scale)
out = Image.new('RGB', (tw * 3, th))
for i, im in enumerate((a, b, d)):
    out.paste(im.resize((tw, th), Image.BILINEAR), (i * tw, 0))
out.save(sys.argv[3])
