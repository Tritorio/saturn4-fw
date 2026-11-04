#!/usr/bin/env python3
"""Convert an image to the exposure LCD stream format (and back), as sent to the display board (step 8.26).

  lcd_stream.py encode IN.png OUT.bin [--fit] [--levels N]
                                                  IN = any image PIL can read (made gray); 15120 x 6230 exactly, or --fit to
                                                  scale it into that size (aspect kept, black borders). --levels N (2..256)
                                                  posterizes the gray values (2 = black / white only: smallest file; the sender
                                                  takes files up to 8 MB). OUT.bin goes to the
                                                  printer's /media/mmcblk0p2/sglcd/<name>.bin (name = a-z 0-9 _ -).
  lcd_stream.py decode IN.bin OUT.png             back to a gray PNG (gray = code -> 0..255, approximately)
  lcd_stream.py quads OUTPREFIX T1 T2 T3 T4 [--base white|marks]
                                                  MULTI EXPOSURE test files: the screen is split into 4 quadrants
                                                  (1 = top-left, 2 = top-right, 3 = bottom-left, 4 = bottom-right as the stream is
                                                  written; split at x = 7560, y = 3115; the corner on the real panel is to be
                                                  confirmed with --base marks) and quadrant i gets T_i seconds. The times are sorted;
                                                  stage k lights the quadrants that are still running and lasts the difference of
                                                  two neighbouring times (equal times share a stage). Writes OUTPREFIX_1.bin ...
                                                  and prints the stages + the matching lcdseq arguments. --base white = lit
                                                  quadrants fully white (the exposure test), marks = a big digit 1-4 in each
                                                  quadrant (orientation check, dim UV, tank out). Pixel counts per quadrant are
                                                  checked by decoding every file again.
  lcd_stream.py selftest                          decode + re-encode the 3 built-in test images (work/fs/customer/resources/
                                                  exposure_test_image0-2.bin) and check that the pixels survive

FORMAT (from the stock test images and the app's image_fpga_compress, docs/exposure_lcd_pipeline.txt):
  48-byte file header (bytes 0-3 = 1d 01 fd 12, bytes 4-7 = the file size, little endian, the rest copied from the test images; the
  board never sees it) + the stream. The stream is cut into 7560-pixel HALF ROWS (a 15120 px row = two halves, 6230 rows):
  per half row  [value byte = 0x80 | ((gray8 >> 1) * 124 // 127)] [run bytes, each 1..72]
  a value byte is ONE pixel; every following run byte repeats that value n more times; a new value byte starts when the
  gray value changes. White = 0xfc, black = 0x80. The sender (libsgtap sgdisp.c) packs the stream in 512-byte pieces "0x3c + 512",
  the last one padded with zeros.  Layers of the print files are NOT converted here: this is for the UV-off / dimmed display test.
"""
import os, struct, sys
import numpy as np

W, H, HALF = 15120, 6230, 7560
MAXRUN = 72                              # longest run byte the stock images use. DO NOT raise it: runs up to 127 are accepted but displays came out distorted
                                          # about half of the time (E2 test ); only 72 was reliable
RES = os.path.join(os.path.dirname(__file__), '..', 'work', 'fs', 'customer', 'resources')


def code_of(g):
    """gray 0..255 -> board code"""
    g = g.astype(np.uint16)
    return (0x80 | ((g >> 1) * 124 // 127)).astype(np.uint8)


def gray_of(c):
    v = (c.astype(np.uint16) & 0x7f) * 127 // 124
    return np.clip(v * 2, 0, 255).astype(np.uint8)


def encode_half(x, out, maxrun=None):
    """one half row of codes (uint8 array) -> bytes appended to out (bytearray)"""
    ch = np.flatnonzero(x[1:] != x[:-1]) + 1
    starts = np.concatenate(([0], ch))
    ends = np.concatenate((ch, [len(x)]))
    vals = x[starts]
    for v, s, e in zip(vals.tolist(), starts.tolist(), ends.tolist()):
        out.append(v)
        r = e - s - 1
        mr = maxrun or MAXRUN
        while r > mr:
            out.append(mr)
            r -= mr
        if r:
            out.append(r)


def encode(codes, maxrun=None):
    assert codes.shape == (H, W)
    out = bytearray()
    for y in range(H):
        encode_half(codes[y, :HALF], out, maxrun)
        encode_half(codes[y, HALF:], out, maxrun)
    return bytes(out)


def header(size):
    ref = open(os.path.join(RES, 'exposure_test_image0.bin'), 'rb').read(48)
    return ref[:4] + struct.pack('<I', size) + ref[8:48]


def decode(stream):
    """stream bytes -> codes (H, W); raises on a stream that is not exactly H x 2 half rows"""
    s = np.frombuffer(stream, dtype=np.uint8)
    isv = s >= 0x80
    isv[0] = True                       # the first byte is a value (the stock images start with 0x00 or 0x80)
    vi = np.flatnonzero(isv)
    # pixels of a segment = 1 + sum of its run bytes
    cs = np.concatenate(([0], np.cumsum(np.where(isv, 0, s.astype(np.int64)))))
    seg_runs = cs[np.append(vi[1:], len(s))] - cs[vi + 1]
    n = 1 + seg_runs
    vals = s[vi]
    flat = np.repeat(vals, n)
    total = W * H
    if len(flat) < total:
        raise ValueError(f'stream gives {len(flat)} pixels, need {total}')
    flat = flat[:total]                 # trailing zero padding is read as runs of 0 and adds nothing
    return flat.reshape(H, W)


SEG = {0: 'abcefg', 1: 'bc', 2: 'abdeg', 3: 'abcdg', 4: 'bcfg', 5: 'acdfg', 6: 'acdefg', 7: 'abc', 8: 'abcdefg', 9: 'abcdfg'}


def digit_mask(n, h, w):
    """seven-segment digit n as a bool array (h, w), thick segments"""
    m = np.zeros((h, w), bool)
    t = w // 6                                   # segment thickness
    mid = h // 2
    box = {'a': (0, t, 0, w), 'd': (h - t, h, 0, w), 'g': (mid - t // 2, mid + t - t // 2, 0, w),
           'f': (0, mid, 0, t), 'b': (0, mid, w - t, w), 'e': (mid, h, 0, t), 'c': (mid, h, w - t, w)}
    for sg in SEG[n]:
        y0, y1, x0, x1 = box[sg]
        m[y0:y1, x0:x1] = True
    return m


def quads(a):
    prefix = a[1]
    try:
        times = [float(x) for x in a[2:6]]
    except ValueError:
        print('times must be numbers (seconds)')
        return 1
    base = a[a.index('--base') + 1] if '--base' in a else 'white'
    if base not in ('white', 'marks') or min(times) <= 0:
        print('--base white|marks, times > 0')
        return 1
    X, Y = W // 2, H // 2
    quad = {1: (slice(0, Y), slice(0, X)), 2: (slice(0, Y), slice(X, W)),
            3: (slice(Y, H), slice(0, X)), 4: (slice(Y, H), slice(X, W))}
    lit = np.zeros((5, H, W), bool)              # lit[q] = the pixels quadrant q shows
    for q, (ys, xs) in quad.items():
        if base == 'white':
            lit[q][ys, xs] = True
        else:                                    # a digit in the middle of the quadrant, 2400 x 1400 px
            dh, dw = 2400, 1400
            y0, x0 = ys.start + (Y - dh) // 2, xs.start + (X - dw) // 2
            lit[q][y0:y0 + dh, x0:x0 + dw] = digit_mask(q, dh, dw)
    order = sorted(range(1, 5), key=lambda q: times[q - 1])
    stages, prev = [], 0.0
    for i, q in enumerate(order):
        t = times[q - 1]
        if t > prev + 1e-9:
            stages.append((t - prev, [r for r in order[i:]]))
            prev = t
    names = []
    for k, (dt, qs) in enumerate(stages, 1):
        g = np.zeros((H, W), np.uint8)
        for q in qs:
            g[lit[q]] = 255
        codes = code_of(g)
        stream = encode(codes)
        stream += bytes((-len(stream)) % 512)
        data = header(48 + len(stream)) + stream
        fn = f'{prefix}_{k}.bin'
        open(fn, 'wb').write(data)
        back = decode(stream) == code_of(np.uint8(255))        # pixels shown white again
        for q in range(1, 5):
            ys, xs = quad[q]
            got, want = int(back[ys, xs].sum()), int(lit[q][ys, xs].sum()) if q in qs else 0
            if got != want:
                print(f'CHECK FAILED {fn}: quadrant {q} lit {got}, expected {want}')
                return 1
        names.append((fn, dt))
        print(f'stage {k}: {fn} {len(data)} bytes, {-(-len(stream) // 512)} packets, lit quadrants {sorted(qs)}, UV on {dt:g} s')
    print('quadrant exposure (UV-on time): ' + ', '.join(f'{q}: {times[q - 1]:g} s' for q in range(1, 5)))
    print('pixel check: OK (every file decoded again, lit pixels per quadrant as planned)')
    base_names = [os.path.basename(f)[:-4] for f, _ in names]
    print('lcdseq UV ' + ' '.join(f'{n} {dt:g}' for n, (f, dt) in zip(base_names, names)))
    return 0


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    if a[0] == 'encode' and len(a) >= 3:
        from PIL import Image
        im = Image.open(a[1]).convert('L')
        if im.size != (W, H):
            if '--fit' not in a:
                print(f'image is {im.size}, need {(W, H)} (or use --fit)')
                return 1
            k = min(W / im.size[0], H / im.size[1])
            im = im.resize((max(1, int(im.size[0] * k)), max(1, int(im.size[1] * k))), Image.LANCZOS)
            bg = Image.new('L', (W, H), 0)
            bg.paste(im, ((W - im.size[0]) // 2, (H - im.size[1]) // 2))
            im = bg
        g = np.asarray(im)
        if '--levels' in a:
            n = max(2, min(256, int(a[a.index('--levels') + 1])))
            g = (np.round(g.astype(np.float32) / 255 * (n - 1)) * 255 / (n - 1)).astype(np.uint8)
        stream = encode(code_of(g))
        pad = (-len(stream)) % 512
        stream += bytes(pad)
        data = header(48 + len(stream)) + stream
        open(a[2], 'wb').write(data)
        print(f'{a[2]}: {len(data)} bytes, {-(-len(stream) // 512)} packets of 512')
        return 0
    if a[0] == 'decode' and len(a) >= 3:
        from PIL import Image
        d = open(a[1], 'rb').read()
        Image.fromarray(gray_of(decode(d[48:]))).save(a[2])
        print('written', a[2])
        return 0
    if a[0] == 'quads' and len(a) >= 7:
        return quads(a)
    if a[0] == 'selftest':
        ok = True
        for i in range(3):
            d = open(os.path.join(RES, f'exposure_test_image{i}.bin'), 'rb').read()
            px = decode(d[48:])
            st = encode(px)
            px2 = decode(st)
            same = np.array_equal(px, px2)
            print(f'test image {i}: {len(d) - 48} stream bytes -> re-encoded {len(st)} ({"same" if same else "DIFFERENT"} pixels)')
            ok &= same
        print('selftest', 'OK' if ok else 'FAILED')
        return 0 if ok else 1
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())
