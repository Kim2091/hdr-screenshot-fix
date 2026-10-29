"""Verify hdr_screenshot_fix outputs. Stdlib only; run with python -I.

usage: verify.py --nvidia-png SRC.png --dumped-icc DUMP.icc
                 [--no-cicp | --cicp] out.png ... [--pixels-json FILE]
                 [--match OURS.icc.png OTHER.icc.png]

  --nvidia-png    NVIDIA HDR PNG whose iCCP payload is the reference profile.
  --dumped-icc    profile written by `hdr_screenshot_fix --dump-icc`.
  --no-cicp/--cicp  expectation for the PNGs that follow (default: --no-cicp).
  --pixels-json   list of {"png", "x", "y", "rgb"} 16-bit code checks (+-1).
  --match         chunk types of OURS must equal those of OTHER, ignoring order.
  --structure OURS REF   ordered chunk sequence must equal REF's (pHYs/tEXt/gAMA ignored).
  --clli PNG MAXCLL MAXFALL   cLLi must match these nit values within 0.05.
  --same-idat A B  concatenated IDAT data of A and B must be identical.
"""
import collections
import hashlib
import json
import struct
import sys
import zlib

PNG_SIG = b'\x89PNG\r\n\x1a\n'
PRE_IDAT_ONLY = {b'cICP', b'iCCP', b'sRGB', b'gAMA', b'cHRM', b'sBIT', b'cLLi', b'mDCV', b'pHYs', b'sPLT',
                 b'PLTE', b'eXIf'}
failures = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        failures.append(msg)


def read_chunks(path):
    data = open(path, 'rb').read()
    assert data[:8] == PNG_SIG, 'bad signature'
    pos, out = 8, []
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        crc, = struct.unpack('>I', data[pos + 8 + n:pos + 12 + n])
        out.append((typ, body, (zlib.crc32(typ + body) & 0xffffffff) == crc))
        pos += 12 + n
    return out


def iccp_profile(chunks):
    body = next(b for t, b, _ in chunks if t == b'iCCP')
    name, rest = body.split(b'\0', 1)
    return name, rest[0], zlib.decompress(rest[1:])


def icc_tag_text(prof, sig):
    cnt, = struct.unpack('>I', prof[128:132])
    for i in range(cnt):
        s, off, ln = struct.unpack('>4sII', prof[132 + 12 * i:144 + 12 * i])
        if s == sig:
            t = prof[off:off + ln]
            l, o = struct.unpack('>II', t[20:28])
            return t[o:o + l].decode('utf-16-be')
    return None


def verify_png(path, ref, expect_cicp):
    print(f'PNG {path}  (expect cICP: {expect_cicp})')
    ch = read_chunks(path)
    check(all(ok for _, _, ok in ch), f'all {len(ch)} chunk CRCs valid')
    types = [t for t, _, _ in ch]
    check(types[0] == b'IHDR' and types[-1] == b'IEND', 'IHDR first, IEND last')
    first_idat = types.index(b'IDAT')
    last_idat = len(types) - 1 - types[::-1].index(b'IDAT')
    check(all(t == b'IDAT' for t in types[first_idat:last_idat + 1]), 'IDAT chunks consecutive')
    bad = [t for t in types[first_idat:] if t in PRE_IDAT_ONLY]
    check(not bad, f'no pre-IDAT-only chunk after IDAT {bad}')
    for t in (b'IHDR', b'iCCP', b'IEND'):
        check(types.count(t) == 1, f'exactly one {t.decode()}')
    check(types.count(b'sRGB') == 0, 'no sRGB chunk')
    idat_sizes = [len(b) for t, b, _ in ch if t == b'IDAT']
    check(max(idat_sizes) <= 65536, f'{len(idat_sizes)} IDAT chunks, largest {max(idat_sizes)} bytes <= 65536')
    if '.icc.' in path:  # PNG mode keeps the source gAMA/cHRM, like the reference tool.
        print(f'  gAMA x{types.count(b"gAMA")}, cHRM x{types.count(b"cHRM")} kept from source')
    else:
        body = {t: b for t, b, _ in ch}
        check(b'gAMA' not in body, 'no gAMA chunk')
        want = [b'IHDR', b'iCCP'] + ([b'cICP'] if expect_cicp else []) + [b'cLLi', b'sBIT', b'cHRM', b'IDAT', b'IEND']
        got = [t for i, t in enumerate(types) if not (t == b'IDAT' and i and types[i - 1] == b'IDAT')]
        check(got == want, 'JXR-mode order ' + ' '.join(t.decode() for t in want))
        check(body.get(b'sBIT') == bytes([16, 16, 16]), 'sBIT = 16,16,16')
        check(body.get(b'cHRM') == struct.pack('>8I', 31270, 32900, 70800, 29200, 17000, 79700, 13100, 4600),
              'cHRM = BT.2020 primaries, D65 white, blue_y 0.046')
    seen = []
    for t in types:
        if t != b'IDAT' and t not in seen:
            seen.append(t)
    print('  chunk order: ' + ' '.join(t.decode() for t in seen) + f' (IDAT x{types.count(b"IDAT")})')
    if expect_cicp:
        cicp = [b for t, b, _ in ch if t == b'cICP']
        check(len(cicp) == 1 and tuple(cicp[0]) == (9, 16, 0, 1), f'one cICP = 9/16/0/1 {[tuple(c) for c in cicp]}')
    else:
        check(b'cICP' not in types, 'no cICP chunk')
    name, method, prof = iccp_profile(ch)
    check(method == 0, f'iCCP name {name.decode()}, compression method {method}')
    check(prof == ref, f'iCCP inflates to {len(prof)} bytes, byte-identical to the NVIDIA profile')
    for t, b, _ in ch:
        if t == b'cLLi':
            cll, fall = struct.unpack('>II', b)
            print(f'  cLLi: MaxCLL {cll / 10000:.1f} nits, MaxFALL {fall / 10000:.1f} nits')


def png_pixels(path, max_y):
    ch = read_chunks(path)
    w, h, bd, ct = struct.unpack('>IIBB', ch[0][1][:10])
    assert bd == 16 and ct == 2, (bd, ct)
    raw = zlib.decompress(b''.join(b for t, b, _ in ch if t == b'IDAT'))
    bpp, stride = 6, w * 6
    assert len(raw) == h * (stride + 1), 'IDAT size does not match IHDR'
    rows, prev, pos = [], bytearray(stride), 0
    for y in range(min(h, max_y + 1)):
        f = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def match(ours, other):
    print(f'MATCH {ours}\n   vs {other}')
    a, b = read_chunks(ours), read_chunks(other)
    ca = collections.Counter(t for t, _, _ in a)
    cb = collections.Counter(t for t, _, _ in b)
    print('  ours : ' + ', '.join(f'{t.decode()} x{n}' for t, n in sorted(ca.items())))
    print('  other: ' + ', '.join(f'{t.decode()} x{n}' for t, n in sorted(cb.items())))
    check(ca == cb, 'same chunk types and counts, ignoring order')
    same = [t.decode() for t in sorted(set(ca) & set(cb))
            if [x for tt, x, _ in a if tt == t] == [x for tt, x, _ in b if tt == t]]
    diff = [t.decode() for t in sorted(set(ca) & set(cb)) if t.decode() not in same]
    print(f'  identical chunk bodies: {", ".join(same)}')
    print(f'  differing chunk bodies: {", ".join(diff) or "none"}')
    for label, chunks in (('ours', a), ('other', b)):
        name, _, prof = iccp_profile(chunks)
        print(f'  {label} iCCP: name {name.decode()}, {len(prof)} bytes, desc {icc_tag_text(prof, b"desc")!r}, '
              f'sha256 {hashlib.sha256(prof).hexdigest()[:16]}')


def structure(path, ignore=(b'pHYs', b'tEXt', b'gAMA')):
    out = []
    for t, _, _ in read_chunks(path):
        if t in ignore or (t == b'IDAT' and out and out[-1] == b'IDAT'):
            continue
        out.append(t)
    return out


def match_structure(ours, ref):
    print(f'STRUCTURE {ours}\n       vs {ref}')
    a, b = structure(ours), structure(ref)
    print('  ours : ' + ' '.join(t.decode() for t in a))
    print('  ref  : ' + ' '.join(t.decode() for t in b) + '  (pHYs/tEXt/gAMA ignored, IDAT runs collapsed)')
    check(a == b, 'same chunk sequence as the working file')
    ra = {t: x for t, x, _ in read_chunks(ours) if t != b'IDAT'}
    rb = {t: x for t, x, _ in read_chunks(ref) if t != b'IDAT'}
    for t in a:
        if t in (b'IDAT', b'IEND'):
            continue
        same = ra.get(t) == rb.get(t)
        print(f'  {t.decode()}: {"identical" if same else "differs"}'
              + ('' if same or t == b'iCCP' else f'  ours {ra[t].hex()} ref {rb[t].hex()}'))


def check_clli(path, cll, fall):
    body = next(b for t, b, _ in read_chunks(path) if t == b'cLLi')
    got_cll, got_fall = (v / 10000 for v in struct.unpack('>II', body))
    check(abs(got_cll - cll) <= 0.05 and abs(got_fall - fall) <= 0.05,
          f'cLLi of {path.split(chr(92))[-1]}: MaxCLL {got_cll:.4f} (want {cll}), MaxFALL {got_fall:.4f} (want {fall})')


def same_idat(a, b):
    sa = b''.join(x for t, x, _ in read_chunks(a) if t == b'IDAT')
    sb = b''.join(x for t, x, _ in read_chunks(b) if t == b'IDAT')
    na = sum(1 for t, _, _ in read_chunks(a) if t == b'IDAT')
    nb = sum(1 for t, _, _ in read_chunks(b) if t == b'IDAT')
    check(sa == sb, f'IDAT stream identical ({len(sa)} bytes) between {a.split(chr(92))[-1]} ({na} IDAT) '
                    f'and {b.split(chr(92))[-1]} ({nb} IDAT)')


def main():
    args = sys.argv[1:]
    ref = dumped = None
    expect_cicp = False
    pixel_checks, matches, structures, clli_checks, idat_checks = [], [], [], [], []
    jobs = []
    while args:
        a = args.pop(0)
        if a == '--nvidia-png':
            ref = iccp_profile(read_chunks(args.pop(0)))[2]
        elif a == '--dumped-icc':
            dumped = open(args.pop(0), 'rb').read()
        elif a == '--cicp':
            expect_cicp = True
        elif a == '--no-cicp':
            expect_cicp = False
        elif a == '--pixels-json':
            pixel_checks += json.load(open(args.pop(0)))
        elif a == '--match':
            matches.append((args.pop(0), args.pop(0)))
        elif a == '--structure':
            structures.append((args.pop(0), args.pop(0)))
        elif a == '--clli':
            clli_checks.append((args.pop(0), float(args.pop(0)), float(args.pop(0))))
        elif a == '--same-idat':
            idat_checks.append((args.pop(0), args.pop(0)))
        else:
            jobs.append((a, expect_cicp))
    print(f'reference profile from NVIDIA PNG: {len(ref)} bytes, desc {icc_tag_text(ref, b"desc")!r}, '
          f'cprt {icc_tag_text(ref, b"cprt")!r}, sha256 {hashlib.sha256(ref).hexdigest()}')
    if dumped is not None:
        check(dumped == ref, '--dump-icc output is byte-identical to the NVIDIA profile')
    for path, ec in jobs:
        verify_png(path, ref, ec)
    decoded = {}
    for pc in pixel_checks:
        if pc['png'] not in decoded:
            decoded[pc['png']] = png_pixels(pc['png'], max(q['y'] for q in pixel_checks if q['png'] == pc['png']))
            print(f'decoded {pc["png"]}')
        w, h, rows = decoded[pc['png']]
        x, y = pc['x'], pc['y']
        got = struct.unpack('>3H', rows[y][x * 6:x * 6 + 6])
        exp = tuple(pc['rgb'])
        check(all(abs(g - e) <= 1 for g, e in zip(got, exp)),
              f'pixel ({x},{y}) {pc.get("label", "")}: got {got}, expected {exp}')
    for o, t in matches:
        match(o, t)
    for o, t in structures:
        match_structure(o, t)
    for p, c, f in clli_checks:
        check_clli(p, c, f)
    for a, b in idat_checks:
        same_idat(a, b)
    print()
    print('ALL CHECKS PASSED' if not failures else f'{len(failures)} CHECK(S) FAILED')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
