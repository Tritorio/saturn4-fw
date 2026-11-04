#!/usr/bin/env python3
"""Tiny ARM reverse-engineering helper for the decompressed U-Boot (no Ghidra needed).

  xref.py IMAGE BASE str  "text"        -> string address + functions referencing it
  xref.py IMAGE BASE dis  ADDR [COUNT]  -> disassemble (ARM mode), strings in literals resolved
  xref.py IMAGE BASE func ADDR          -> disassemble the whole function containing ADDR
  xref.py IMAGE BASE calls ADDR         -> functions that call (bl) the function at ADDR
"""
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs

img = open(sys.argv[1], "rb").read()
BASE = int(sys.argv[2], 16)
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
END = BASE + len(img)


def word(addr):
    o = addr - BASE
    return struct.unpack_from("<I", img, o)[0] if 0 <= o <= len(img) - 4 else None


def cstr(addr):
    o = addr - BASE
    if not 0 <= o < len(img):
        return None
    e = img.find(b"\0", o, o + 200)
    s = img[o:e]
    if len(s) >= 3 and all(32 <= c < 127 or c in (9, 10, 13, 27) for c in s):
        return s.decode("ascii", "replace")
    return None


def func_start(addr):
    """Walk back to a 'push {..., lr}' / stmdb sp!, {..lr} (ARM)."""
    a = addr & ~3
    for _ in range(4000):
        w = word(a)
        if w is not None and (w & 0xFFFF4000) == 0xE92D4000:
            return a
        a -= 4
    return None


def func_end(start):
    a = start + 4
    while a < END:
        w = word(a)
        # pop {..., pc} / ldmia sp!, {..pc}
        if w is not None and (w & 0xFFFF8000) == 0xE8BD8000:
            nxt = word(a + 4)
            if nxt is not None and (nxt & 0xFFFF4000) == 0xE92D4000:
                return a + 4
            # keep going if more code follows inside the function (multiple returns)
            j = a + 4
            while j < END and word(j) is not None and (word(j) & 0xFFFF4000) != 0xE92D4000 and j - a < 0x400:
                j += 4
            return j
        a += 4
    return END


def annotate(ins):
    extra = ""
    m = re.search(r"\[pc, #(-?0x[0-9a-f]+|-?\d+)\]", ins.op_str)
    if ins.mnemonic.startswith("ldr") and m:
        lit = ins.address + 8 + int(m.group(1), 0)
        v = word(lit)
        if v is not None:
            s = cstr(v)
            extra = f"  ; ={v:#x}" + (f' "{s}"' if s else "")
    return extra


def dis(addr, count):
    o = addr - BASE
    for ins in md.disasm(img[o:o + count * 4], addr):
        print(f"{ins.address:08x}: {ins.mnemonic:<8} {ins.op_str}{annotate(ins)}")


def refs_to(value):
    pat = struct.pack("<I", value)
    out, i = [], img.find(pat)
    while i >= 0:
        if i % 4 == 0:
            out.append(BASE + i)
        i = img.find(pat, i + 1)
    return out


def code_refs_to_literal(lit_addr):
    """ldr rX, [pc, #imm] instructions within 4 KB before the literal that load it."""
    out = []
    for a in range(max(BASE, lit_addr - 4096), lit_addr, 4):
        w = word(a)
        if w is None:
            continue
        if (w & 0x0F7F0000) == 0x051F0000:  # ldr Rt, [pc, #+/-imm12]
            imm = w & 0xFFF
            tgt = a + 8 + (imm if w & 0x00800000 else -imm)
            if tgt == lit_addr:
                out.append(a)
    return out


cmd = sys.argv[3]
if cmd == "str":
    needle = sys.argv[4].encode()
    i = img.find(needle)
    while i >= 0:
        sa = BASE + i
        print(f"string @ {sa:#x}: {cstr(sa)!r}")
        for lit in refs_to(sa):
            for c in code_refs_to_literal(lit):
                fs = func_start(c)
                print(f"   used at {c:#x} (function {fs:#x})" if fs else f"   used at {c:#x}")
        i = img.find(needle, i + 1)
elif cmd == "dis":
    dis(int(sys.argv[4], 16), int(sys.argv[5]) if len(sys.argv) > 5 else 40)
elif cmd == "func":
    s = func_start(int(sys.argv[4], 16))
    e = func_end(s)
    print(f"; function {s:#x} .. {e:#x}")
    dis(s, (e - s) // 4)
elif cmd == "calls":
    tgt = int(sys.argv[4], 16)
    for a in range(BASE, END - 4, 4):
        w = word(a)
        if (w & 0x0F000000) == 0x0B000000:  # bl / blx imm (any cond)
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 0x1000000
            if a + 8 + off * 4 == tgt:
                fs = func_start(a)
                print(f"call at {a:#x} in function {fs:#x}" if fs else f"call at {a:#x}")
