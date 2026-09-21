# Oracle runs side by side: the first CSV (r_swStatFile) defines the slow frames (back end above the
# budget); every file's means are printed over ALL frames and over THAT frame set.
#   python tools\swstat-compare.py base.csv cutA.csv cutB.csv [--budget 23.2] [--cols total,colour,light]
import csv, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = lambda name, default: sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
budget = float(opt('--budget', '23.2'))
cols = opt('--cols', 'total,tiles,shadow,light,colour,setup').split(',')
files = []
for a in list(args):
    if a.endswith('.csv'):
        files.append(a)
load = lambda p: [{k: float(v) for k, v in r.items()} for r in csv.DictReader(open(p))]
base = load(files[0])
# the frame as the player gets it, once the file has it (the tile pass overlaps the next frame: total is the main thread's share)
by = 'period' if 'period' in base[0] else 'total'
if by == 'period' and '--budget' not in sys.argv:
    budget = 25.0
slow = {int(r['frame']) for r in base if r[by] > budget}
print('%d slow frames of %d in %s' % (len(slow), len(base), files[0]))
print('%-28s' % 'file' + ''.join('%18s' % c for c in cols))
for f in files:
    rows = load(f)
    s = [r for r in rows if int(r['frame']) in slow]
    cell = lambda c: '%8.2f /%7.2f' % (sum(r[c] for r in rows) / len(rows), sum(r[c] for r in s) / max(1, len(s)))
    print('%-28s' % f.replace('\\', '/').split('/')[-1] + ''.join('%18s' % cell(c) for c in cols))
print('(each cell: mean over all frames / mean over the slow set)')
