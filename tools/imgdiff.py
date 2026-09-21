# Compares two captures: PSNR, largest difference, and the worst 128-pixel window (plan 8.1:
# never PSNR alone). Pillow only, no numpy on this machine.
#
#   python tools\imgdiff.py a.tga b.tga                 one pair
#   python tools\imgdiff.py a.tga b.tga --crop out.png  also writes  a | b | 8x difference  of the worst window
#   python tools\imgdiff.py --set gl-matched gl-matched2 [--dir build\save-auto\base\demoshots]
#         every frame that exists under both tags
import argparse
import glob
import math
import os
import re

from PIL import Image, ImageChops, ImageStat

WINDOW = 128


def psnr(mse):
    return 99.0 if mse <= 0 else 10.0 * math.log10(255.0 * 255.0 / mse)


def compare(path_a, path_b, crop=None):
    a = Image.open(path_a).convert('RGB')
    b = Image.open(path_b).convert('RGB')
    if a.size != b.size:
        raise SystemExit(f'size mismatch: {a.size} vs {b.size}')
    diff = ImageChops.difference(a, b)
    mse = sum(ImageStat.Stat(diff).sum2) / (3.0 * a.width * a.height)
    largest = max(hi for _, hi in diff.getextrema())
    differing = sum(1 for p in diff.convert('L').point(lambda v: 255 if v else 0).getdata() if p)

    # worst window on a half-window grid: squared error, box-filtered
    sq = diff.convert('L').point(lambda v: min(255, v * v))      # saturates at |d| = 16: ranking only
    worst, wx, wy = -1.0, 0, 0
    step = WINDOW // 2
    for y in range(0, max(1, a.height - WINDOW + 1), step):
        for x in range(0, max(1, a.width - WINDOW + 1), step):
            m = ImageStat.Stat(sq.crop((x, y, x + WINDOW, y + WINDOW))).mean[0]
            if m > worst:
                worst, wx, wy = m, x, y
    box = (wx, wy, min(a.width, wx + WINDOW), min(a.height, wy + WINDOW))
    wdiff = diff.crop(box)
    wmse = sum(ImageStat.Stat(wdiff).sum2) / (3.0 * wdiff.width * wdiff.height)

    if crop:
        scale = 3
        sheet = Image.new('RGB', (3 * WINDOW * scale, WINDOW * scale))
        for i, img in enumerate((a.crop(box), b.crop(box), wdiff.point(lambda v: min(255, v * 8)))):
            sheet.paste(img.resize((WINDOW * scale, WINDOW * scale), Image.NEAREST), (i * WINDOW * scale, 0))
        sheet.save(crop)

    return {'psnr': psnr(mse), 'max': largest, 'differing': differing / float(a.width * a.height),
            'window': (wx, wy), 'window_psnr': psnr(wmse)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a', nargs='?')
    ap.add_argument('b', nargs='?')
    ap.add_argument('--crop')
    ap.add_argument('--set', nargs=2, metavar=('TAG_A', 'TAG_B'))
    ap.add_argument('--dir', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'save-auto', 'base', 'demoshots'))
    args = ap.parse_args()

    if args.set:
        tag_a, tag_b = args.set
        print(f"{'frame':<8}{'PSNR dB':>9}{'max':>5}{'differing':>11}{'worst window':>16}{'its PSNR':>10}")
        for path_a in sorted(glob.glob(os.path.join(args.dir, f'*_{tag_a}_*_f*.tga'))):
            path_b = path_a.replace(f'_{tag_a}_', f'_{tag_b}_')
            if not os.path.exists(path_b):
                continue
            frame = re.search(r'_f(\d+)\.tga$', path_a).group(1)
            crop = None
            if args.crop:
                crop = os.path.join(args.crop, f'diff_{tag_a}_vs_{tag_b}_f{frame}.png')
            r = compare(path_a, path_b, crop)
            print(f"{frame:<8}{r['psnr']:>9.2f}{r['max']:>5}{r['differing'] * 100:>10.3f}%{str(r['window']):>16}{r['window_psnr']:>10.2f}")
    else:
        r = compare(args.a, args.b, args.crop)
        print(f"PSNR {r['psnr']:.2f} dB  max {r['max']}  differing {r['differing'] * 100:.3f}%  "
              f"worst {WINDOW}px window at {r['window']}: {r['window_psnr']:.2f} dB")


main()
