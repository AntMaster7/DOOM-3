# Summarizes an r_frameLog CSV: mean, median, p95, max per column, for the timed demo pass.
#
#   python tools\census-summary.py build\save-auto\base\census-demo1-1920x1080.csv
#
# A "twice" timedemo writes the warm-up pass and the timed pass into one file; the demoFrame column
# restarts at the second pass, and only that pass is summarized. No numpy on this machine.
import csv
import sys


def main():
    path = sys.argv[1]
    rows = list(csv.DictReader(open(path, newline='')))
    rows = [r for r in rows if int(r['demoFrame']) > 0]

    # keep the last pass: find the last place where demoFrame goes backwards
    start = 0
    for i in range(1, len(rows)):
        if int(rows[i]['demoFrame']) < int(rows[i - 1]['demoFrame']):
            start = i
    rows = rows[start:]
    print(f'{path}: {len(rows)} frames in the timed pass')

    cols = [c for c in rows[0].keys() if c not in ('frameCount', 'demoFrame')]
    print(f"{'column':<18}{'mean':>12}{'median':>12}{'p95':>12}{'max':>12}  at demoFrame")
    for c in cols:
        vals = sorted(float(r[c]) for r in rows)
        n = len(vals)
        mean = sum(vals) / n
        worst = max(rows, key=lambda r: float(r[c]))
        print(f"{c:<18}{mean:>12.2f}{vals[n // 2]:>12.2f}{vals[int(n * 0.95)]:>12.2f}{vals[-1]:>12.2f}  {worst['demoFrame']}")

    # derived: what the pass structure costs per frame, in units the software renderer cares about
    px = float(1920 * 1080)
    for label, col in (('light scissor area / screen', 'lightPx'),
                       ('lit surface scissor area / screen', 'litSurfPx'),
                       ('shadow surface scissor area / screen', 'shadowPx')):
        vals = sorted(float(r[col]) / px for r in rows)
        n = len(vals)
        print(f'{label:<40} mean {sum(vals) / n:6.2f}x  p95 {vals[int(n * 0.95)]:6.2f}x  max {vals[-1]:6.2f}x')

    heavy = sorted(rows, key=lambda r: float(r['litTris']) + float(r['shadowTris']) + float(r['tris']), reverse=True)[:5]
    print('heaviest frames by triangles submitted (tris + litTris + shadowTris):')
    for r in heavy:
        print(f"  demoFrame {r['demoFrame']:>5}: tris {r['tris']:>7} litTris {r['litTris']:>7} shadowTris {r['shadowTris']:>7} lights {r['lights']:>3}")


main()
