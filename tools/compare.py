"""1) icc mode: every non-colour chunk of the source PNG survives byte for byte, in order.
2) jxr mode vs NVIDIA's own PQ PNG of the same capture: per-channel code difference over rows 0..N.

usage: compare.py src.png src.icc.png ours_from_jxr.png nvidia.png rows
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from verify import read_chunks, png_pixels  # noqa: E402

src, icc, ours, nv, rows = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
colour = {b'iCCP', b'sRGB', b'gAMA', b'cHRM'}
a = [(t, b) for t, b, _ in read_chunks(src) if t not in colour]
b = [(t, d) for t, d, _ in read_chunks(icc) if t not in colour]
print('non-colour chunks identical and in order:', a == b, f'({len(a)} chunks)')
print('removed from source:', sorted({t.decode() for t, _, _ in read_chunks(src) if t in colour}))

_, _, ra = png_pixels(ours, rows - 1)
_, _, rb = png_pixels(nv, rows - 1)
import struct
hist = {}
mx = 0
n = 0
for y in range(rows):
    va = struct.unpack(f'>{len(ra[y]) // 2}H', ra[y])
    vb = struct.unpack(f'>{len(rb[y]) // 2}H', rb[y])
    for p, q in zip(va, vb):
        d = abs(p - q)
        mx = max(mx, d)
        k = 0 if d == 0 else 1 if d <= 64 else 2 if d <= 256 else 3
        hist[k] = hist.get(k, 0) + 1
        n += 1
names = ['equal', '1..64 (<0.1%)', '65..256', '>256']
print(f'ours vs NVIDIA PNG, rows 0..{rows - 1}, {n} channel values, max diff {mx} of 65535:')
for k in range(4):
    print(f'  {names[k]:>14}: {hist.get(k, 0) / n * 100:.3f}%')
