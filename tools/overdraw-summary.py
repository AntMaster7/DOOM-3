# Summarizes the per-frame "overdraw: N" lines that r_showLightCount 3/4 and r_showShadowCount 2
# print into a console log (RB_CountStencilBuffer: mean stencil count over the whole window).
#
#   python tools\overdraw-summary.py build\save-auto\base\bench-lightcount-visible-*.log
import glob
import re
import sys

for pattern in sys.argv[1:]:
    for path in glob.glob(pattern):
        vals = [float(m.group(1)) for m in re.finditer(r'overdraw:\s*([0-9.]+)', open(path, errors='replace').read())]
        if not vals:
            print(f'{path}: no overdraw lines')
            continue
        s = sorted(vals)
        n = len(s)
        worst = vals.index(s[-1])
        print(f'{path}\n  frames {n}  mean {sum(s) / n:.2f}  median {s[n // 2]:.2f}  p95 {s[int(n * 0.95)]:.2f}  '
              f'max {s[-1]:.2f} (line index {worst})')
