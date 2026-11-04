#!/usr/bin/env python3
"""ARM-mode xref helper for chitu (text vaddr 0x10000 = file 0).
  ax.py BIN ref ADDR [ADDR..]   movw/movt (+ add #imm) sites producing ADDR
  ax.py BIN dis ADDR N          disassembly, movw/movt values + strings resolved
  ax.py BIN func ADDR           whole function containing ADDR
  ax.py BIN calls ADDR          bl sites calling ADDR
"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
B = sys.argv[1]; img = open(B, 'rb').read(); BASE = 0x10000; TEXT_END = 0x2b14b4
DBASE, DOFF = 0x2d168c, 0x2b168c
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
def w(a):
    o = a - DBASE + DOFF if a >= DBASE else a - BASE
    return struct.unpack_from('<I', img, o)[0] if 0 <= o < len(img) - 4 else None
def cstr(a):
    o = a - BASE
    if not 0 <= o < TEXT_END - BASE: return None
    e = img.find(b'\0', o, o + 200); s = img[o:e]
    if len(s) >= 2 and all(32 <= c < 127 or c in (9, 10) for c in s): return s.decode()
    return None
def scan():
    lo = {}; full = {}
    for o in range(0, TEXT_END - BASE, 4):
        a = o + BASE; x = struct.unpack_from('<I', img, o)[0]
        if (x & 0x0FF00000) == 0x03000000:
            rd = (x >> 12) & 15; lo[rd] = (a, ((x >> 4) & 0xF000) | (x & 0xFFF)); full.pop(rd, None)
        elif (x & 0x0FF00000) == 0x03400000:
            rd = (x >> 12) & 15
            if rd in lo and a - lo[rd][0] <= 64:
                v = ((((x >> 4) & 0xF000) | (x & 0xFFF)) << 16) | lo[rd][1]; full[rd] = (a, v); yield a, rd, v
        elif (x & 0x0FF00000) == 0x02800000:     # add rd, rn, #imm
            rn = (x >> 16) & 15; rd = (x >> 12) & 15
            rot = ((x >> 8) & 15) * 2; imm = x & 0xFF; imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF
            if rn in full and a - full[rn][0] <= 64:
                v = (full[rn][1] + imm) & 0xFFFFFFFF; yield a, rd, v
def func_start(a):
    a &= ~3
    for _ in range(20000):
        x = w(a)
        if x is not None and (x & 0xFFFF4000) == 0xE92D4000: return a
        a -= 4
def dis(a, n):
    vals = {}
    for i in md.disasm(img[a - BASE:a - BASE + n * 4], a):
        cm = ''; x = struct.unpack_from('<I', img, i.address - BASE)[0]
        if (x & 0x0FF00000) == 0x03400000:
            rd = (x >> 12) & 15; prev = w(i.address - 4)
        for aa, rd, v in []: pass
        print(f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}', end='')
        if i.mnemonic in ('movw', 'movt', 'add'):
            r = i.op_str.split(',')[0]
            if i.mnemonic == 'movw': vals[r] = int(i.op_str.split('#')[1], 0)
            elif i.mnemonic == 'movt' and r in vals:
                vals[r] = (int(i.op_str.split('#')[1], 0) << 16) | (vals[r] & 0xFFFF); s = cstr(vals[r])
                print(f'  ; {vals[r]:#x} {s!r}' if s else f'  ; {vals[r]:#x}', end='')
            elif i.mnemonic == 'add' and '#' in i.op_str:
                p = [q.strip() for q in i.op_str.split(',')]
                if p[1] in vals:
                    v = vals[p[1]] + int(p[2][1:], 0); vals[p[0]] = v; s = cstr(v)
                    print(f'  ; {v:#x} {s!r}' if s else f'  ; {v:#x}', end='')
        print()
def calls(t):
    r = []
    for o in range(0, TEXT_END - BASE, 4):
        x = struct.unpack_from('<I', img, o)[0]
        if (x & 0x0F000000) == 0x0B000000:
            off = x & 0xFFFFFF
            if off & 0x800000: off -= 1 << 24
            if o + BASE + 8 + off * 4 == t: r.append(o + BASE)
    return r
c = sys.argv[2]
if c == 'ref':
    ts = {int(x, 16) for x in sys.argv[3:]}
    for a, rd, v in scan():
        if v in ts: print(f'{v:#x} at {a:#x} r{rd}  func {func_start(a):#x}')
elif c == 'dis': dis(int(sys.argv[3], 16), int(sys.argv[4]))
elif c == 'func':
    s = func_start(int(sys.argv[3], 16)); n = int(sys.argv[4]) if len(sys.argv) > 4 else 400
    print(f'func {s:#x}'); dis(s, n)
elif c == 'calls': print([f'{x:#x} (func {func_start(x):#x})' for x in calls(int(sys.argv[3], 16))])
if c == 'field':
    # base via movw/movt, then ldr/str{b,h} [rN, #off] within 40 insns using that reg
    base = int(sys.argv[3], 16); offs = {int(x, 16) for x in sys.argv[4:]}
    for a, rd, v in scan():
        if v != base: continue
        for k in range(1, 40):
            o = a - BASE + 4 * k; x = struct.unpack_from('<I', img, o)[0]
            for i in md.disasm(img[o:o + 4], o + BASE):
                if i.mnemonic[:3] in ('ldr', 'str') and i.op_str.find(f'[r{rd}, #') >= 0 or (i.op_str.find(f'[{["r0","r1","r2","r3","r4","r5","r6","r7","r8","sb","sl","fp","ip","sp","lr","pc"][rd]}, #') >= 0 and i.mnemonic[:3] in ('ldr','str')):
                    try: off = int(i.op_str.split('#')[1].rstrip(']!'), 0)
                    except: continue
                    if off in offs: print(f'{i.address:#x} {i.mnemonic} {i.op_str}   func {func_start(i.address):#x}')
