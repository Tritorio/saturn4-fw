#!/usr/bin/env python3
"""Check + preview the layer image dumps of the board link tracer (sgcap/fpga_img_<t0 ms>_<n>p.bin, FPGADUMP flag).

  fpga_layer_dump.py FILE.bin [...]     per file: strict half-row check (12460 half rows of exactly 7560 px, every half row starts with a
                                        value byte, longest run byte), lit pixels per quadrant (split x = 7560, y = 3115), content within
                                        40 px of the centre lines, and a 756 x 311 preview PNG next to the file.
The quadrants are whole half rows, so a quadrant can be blanked by replacing its half rows."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import numpy as np
import lcd_stream as L
from PIL import Image
Image.MAX_IMAGE_PIXELS = None


def half_rows(s):
    i, n, out = 0, len(s), []
    while i < n and len(out) < 12460:
        st, px = i, 1
        i += 1
        while px < 7560 and i < n:
            px += 1 if s[i] >= 0x80 else s[i]
            i += 1
        out.append((st, i, px))
    return out


def main(files):
    for f in files:
        s = open(f, 'rb').read()
        hr = half_rows(s)
        arr = np.frombuffer(s, np.uint8)
        print(f"{os.path.basename(f)}: half rows {len(hr)}, with px != 7560: {sum(1 for a, b, p in hr if p != 7560)}, "
              f"starting with a run byte: {sum(1 for a, b, p in hr if s[a] < 0x80)}, longest run byte {int(arr[arr < 0x80].max())}, tail {len(s) - hr[-1][1]} B")
        g = L.gray_of(L.decode(s))
        h, w = g.shape
        x, y = w // 2, h // 2
        q = {1: g[:y, :x], 2: g[:y, x:], 3: g[y:, :x], 4: g[y:, x:]}
        print('  lit px per quadrant (1 TL, 2 TR, 3 BL, 4 BR):', {k: int((v > 0).sum()) for k, v in q.items()})
        print('  lit px within 40 px of the centre lines: x', int((g[:, x - 40:x + 40] > 0).sum()), ' y', int((g[y - 40:y + 40, :] > 0).sum()))
        Image.fromarray(g).resize((756, 311), Image.BOX).save(os.path.splitext(f)[0] + '.png')


if __name__ == '__main__':
    main(sys.argv[1:]) if len(sys.argv) > 1 else print(__doc__)
