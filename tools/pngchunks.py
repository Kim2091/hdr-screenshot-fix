"""List PNG chunks with lengths, CRC status and decoded cICP. Stdlib only."""
import struct
import sys
import zlib


def chunks(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos = 8
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        crc, = struct.unpack('>I', data[pos + 8 + n:pos + 12 + n])
        ok = (zlib.crc32(typ + body) & 0xffffffff) == crc
        yield typ.decode('latin-1'), body, ok
        pos += 12 + n


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(p)
        idat = 0
        for typ, body, ok in chunks(p):
            if typ == 'IDAT':
                idat += 1
                continue
            extra = ''
            if typ == 'cICP':
                extra = ' ' + '/'.join(str(b) for b in body)
            elif typ == 'IHDR':
                w, h, bd, ct = struct.unpack('>IIBB', body[:10])
                extra = f' {w}x{h} depth {bd} colortype {ct}'
            elif typ == 'iCCP':
                extra = ' name=' + body.split(b'\0')[0].decode('latin-1')
            print(f'  {typ} len={len(body)} crc={"ok" if ok else "BAD"}{extra}')
        print(f'  IDAT x{idat}')
