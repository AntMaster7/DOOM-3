# What the slow frames of a run spend their time on: reads an r_swStatFile CSV (one row per frame)
# and compares the frames whose back end exceeds a budget with the rest.
#   python tools\swstat-slow.py build\save-auto\base\swstat.csv [--budget 23.2]
# 23.2 ms = 40 fps minus the front end's 1.0 ms mean and a little air.
import csv, sys

path = sys.argv[1]
budget = float(sys.argv[sys.argv.index('--budget') + 1]) if '--budget' in sys.argv else 23.2
rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(open(path))]
# --by period: the frame as the player gets it (needed once the tile pass overlaps the next frame: total is then
# only the main thread's share). 25 ms = 40 fps.
by = sys.argv[sys.argv.index('--by') + 1] if '--by' in sys.argv else 'total'
if by == 'period' and '--budget' not in sys.argv:
    budget = 25.0
slow = [r for r in rows if r[by] > budget]
fast = [r for r in rows if r[by] <= budget]
print('%d frames, %d (%.1f%%) with %s above %.1f ms' % (len(rows), len(slow), 100.0 * len(slow) / max(1, len(rows)), by, budget))
if not slow or not fast:
    sys.exit(0)
cols = ['total', 'submit', 'xform', 'setup', 'sort', 'tiles', 'present', 'load', 'depth', 'shadow', 'light', 'colour', 'store',
        'tris', 'mpxDepth', 'mpxStencil', 'mpxColor', 'mpxLightAsked', 'mpxOtherAsked', 'mpxOther2D', 'kOtherCalls',
        'st2dRep', 'st2dAdd', 'st2dBlend', 'st3dRep', 'st3dAdd', 'st3dBlend', 'stKCalls', 'stKWin', 'stKVertexColor']
if 'period' in rows[0]:
    # what the main thread spends OUTSIDE the back end (game, front end): the ceiling of overlapping the two
    for r in rows:
        r['outside'] = r['period'] - r['total'] if r['period'] > 0 else 0.0
    cols = ['period', 'outside'] + cols
cols = [c for c in cols if c in rows[0]]
mean = lambda rs, c: sum(r[c] for r in rs) / len(rs)
print('%-14s %10s %10s %10s' % ('', 'slow', 'the rest', 'difference'))
for c in cols:
    a, b = mean(slow, c), mean(fast, c)
    print('%-14s %10.2f %10.2f %+10.2f' % (c, a, b, a - b))
# where in the demo they are: runs of consecutive slow frames
runs, start, prev = [], None, None
for r in rows:
    f = int(r['frame'])
    if r[by] > budget:
        if start is None:
            start = f
        prev = f
    elif start is not None:
        runs.append((start, prev)); start = None
if start is not None:
    runs.append((start, prev))
runs.sort(key=lambda ab: ab[0] - ab[1])
print('longest slow runs (frame index in the file):', ', '.join('%d-%d' % ab for ab in runs[:8]))
