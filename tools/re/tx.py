#!/usr/bin/env python3
"""Thumb xref helper for chitu (EXEC, text at file off 0 = vaddr 0x10000).
  tx.py BIN str TEXT       -> refs (ldr lit + add pc) to the string
  tx.py BIN dis ADDR N     -> thumb disassembly with resolved pc-rel strings
  tx.py BIN calls ADDR     -> bl/blx sites to ADDR
  tx.py BIN plt NAME       -> PLT address of an imported function
"""
import struct, sys, subprocess, re
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
B = sys.argv[1]; img = open(B, 'rb').read(); BASE = 0x10000
TEXT_END = 0x2b14b4
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.detail = False
def h(a): return struct.unpack_from('<H', img, a - BASE)[0]
def w(a):
    o = a - BASE
    return struct.unpack_from('<I', img, o)[0] if 0 <= o < len(img) - 4 else None
def cstr(a):
    o = a - BASE
    if not 0 <= o < TEXT_END - BASE: return None
    e = img.find(b'\0', o, o + 300); s = img[o:e]
    if len(s) >= 2 and all(32 <= c < 127 or c in (9, 10) for c in s): return s.decode()
    return None
def refs(target):
    out = []
    for o in range(0, TEXT_END - BASE, 2):
        v = struct.unpack_from('<H', img, o)[0]
        if (v & 0xFF78) == 0x4478:          # add rd, pc  (rd<8)
            rd = v & 7; a = o + BASE
            for back in range(2, 24, 2):     # look for ldr rd,[pc,#imm]
                u = h(a - back)
                if (u & 0xF800) == 0x4800 and ((u >> 8) & 7) == rd:
                    la = a - back; lit = ((la + 4) & ~3) + (u & 0xFF) * 4
                    val = w(lit)
                    if val is not None and ((val + a + 4) & 0xFFFFFFFF) == target: out.append(a)
                    break
    return out
def dis(a, n):
    code = img[a - BASE:a - BASE + n * 4]
    regs = {}
    for i in md.disasm(code, a):
        cm = ''
        if i.mnemonic.startswith('ldr') and '[pc' in i.op_str:
            m = re.search(r'#(-?0x[0-9a-f]+|-?\d+)', i.op_str)
            imm = int(m.group(1), 0) if m else 0
            lit = ((i.address + 4) & ~3) + imm; val = w(lit)
            rd = i.op_str.split(',')[0]; regs[rd] = val; cm = f'  ; ={val:#x}' if val is not None else ''
        elif i.mnemonic == 'add' and i.op_str.endswith(', pc') and i.size == 2:
            rd = i.op_str.split(',')[0]
            if regs.get(rd) is not None:
                t = (regs[rd] + i.address + 4) & 0xFFFFFFFF; s = cstr(t)
                cm = f'  ; -> {t:#x} ' + (repr(s) if s else '')
        print(f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}{cm}')
def calls(t):
    out = []
    for o in range(0, TEXT_END - BASE - 4, 2):
        a = o + BASE; hi, lo = h(a), h(a + 2)
        if (hi & 0xF800) == 0xF000 and (lo & 0xC000) == 0xC000:
            S = (hi >> 10) & 1; imm10 = hi & 0x3FF; J1 = (lo >> 13) & 1; J2 = (lo >> 11) & 1; imm11 = lo & 0x7FF
            I1 = 1 - (J1 ^ S); I2 = 1 - (J2 ^ S)
            off = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1)
            if S: off -= 1 << 25
            dst = a + 4 + off
            if not (lo & 0x1000): dst &= ~3   # blx -> arm
            if dst == t: out.append(a)
    return out
cmd = sys.argv[2]
if cmd == 'str':
    s = sys.argv[3].encode() + b'\0'; i = -1
    while True:
        i = img.find(s, i + 1)
        if i < 0: break
        if i > 0 and img[i - 1] != 0: continue
        print(f'string @{i + BASE:#x}:', [hex(x) for x in refs(i + BASE)])
elif cmd == 'dis': dis(int(sys.argv[3], 16), int(sys.argv[4]))
elif cmd == 'calls': print([hex(x) for x in calls(int(sys.argv[3], 16))])
elif cmd == 'plt':
    o = subprocess.run(['objdump', '-d', '-j', '.plt', B], capture_output=True, text=True).stdout
    r = subprocess.run(['readelf', '-rW', B], capture_output=True, text=True).stdout
    for l in r.splitlines():
        if sys.argv[3] in l: print(l)
def movwt():
    """yield (addr_of_movt, rd, value) for movw/movt pairs"""
    last = {}
    for o in range(0, TEXT_END - BASE - 4, 2):
        a = o + BASE; hi, lo = h(a), h(a + 2)
        if (hi & 0xFB70) in (0xF240, 0xF2C0) and not (lo & 0x8000):
            imm = ((hi & 0xF) << 12) | (((hi >> 10) & 1) << 11) | (((lo >> 12) & 7) << 8) | (lo & 0xFF)
            rd = (lo >> 8) & 0xF
            if (hi & 0xFB70) == 0xF240: last[rd] = (a, imm)
            elif rd in last and a - last[rd][0] <= 24:
                yield a, rd, (imm << 16) | last[rd][1]
if cmd == 'abs':
    ts = {int(x, 16) for x in sys.argv[3:]}
    for a, rd, v in movwt():
        if v in ts: print(f'{v:#x} loaded at {a:#x} r{rd}')
if cmd == 'range':
    lo_, hi_ = int(sys.argv[3], 16), int(sys.argv[4], 16)
    for a, rd, v in movwt():
        if lo_ <= v < hi_: print(f'movw/movt {v:#x} at {a:#x} r{rd}')
    import struct as S
    for o in range(0, len(img) - 4, 4):
        v = S.unpack_from('<I', img, o)[0]
        if lo_ <= v < hi_: print(f'word {v:#x} at file {o:#x}')
