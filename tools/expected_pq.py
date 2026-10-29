"""Turn scRGB reference pixels into expected 16-bit PQ codes (independent of the C++ tool).

usage: expected_pq.py values.json out.png checks.json [label]
Appends {"png", "x", "y", "rgb", "label"} entries to checks.json.
"""
import json
import os
import sys

M = [[0.627403895934699, 0.329283038377884, 0.043313065687417],
     [0.069097289358232, 0.919540395075459, 0.011362315566309],
     [0.016391438875150, 0.088013307877226, 0.895595253247624]]
m1, m2 = 2610 / 16384, 2523 / 4096 * 128
c1, c2, c3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq(y):
    p = max(y, 0.0) ** m1
    return ((c1 + c2 * p) / (1 + c3 * p)) ** m2


def code(rgb):
    out = []
    for row in M:
        v = sum(a * b for a, b in zip(row, rgb))
        nits = min(max(v, 0.0) * 80.0, 10000.0)
        out.append(round(pq(nits / 10000.0) * 65535))
    return out


vals = json.load(open(sys.argv[1], encoding='utf-8-sig'))
png, dst = sys.argv[2], sys.argv[3]
label = sys.argv[4] if len(sys.argv) > 4 else ''
checks = json.load(open(dst)) if os.path.exists(dst) else []
for v in vals:
    checks.append({'png': png, 'x': v['x'], 'y': v['y'], 'rgb': code(v['rgb']),
                   'label': f'{label} scRGB {[round(c, 4) for c in v["rgb"]]}'})
json.dump(checks, open(dst, 'w'), indent=1)
print(f'{len(vals)} checks for {png}')
