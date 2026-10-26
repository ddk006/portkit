"""bluebox eurorack 1.5.1 (BLUEEURO.BIN): three images in one file, address helpers, Thumb disassembly, literal refs.

  python3 tools/bbx.py dis ADDR [N]        disassemble N instructions at ADDR
  python3 tools/bbx.py lit VALUE...        literal-pool slots holding VALUE and the LDRs that load them
  python3 tools/bbx.py str TEXT            addresses of a string and the LDR/ADR refs to it
  python3 tools/bbx.py func ADDR           guess the enclosing function start (last push before ADDR)
"""
import os, struct, sys, hashlib
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from capstone.arm import ARM_OP_MEM, ARM_REG_PC

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware", "BLUEEURO.BIN")
SHA = "fcb04565e2fd5bc15c0b2d0dc913dc99dc74732611e8cef0d7e8e4bb4af7402b"
data = open(FW, "rb").read()
assert hashlib.sha256(data).hexdigest() == SHA, "BLUEEURO.BIN is not stock 1.5.1"
# (flash base, file offset, size): M7 main, 0x080E0000 block, M4 (same split as bluelex patch.py)
IMAGES = [(0x08040000, 0x000000, 0xA0000), (0x080E0000, 0x0A0000, 0x20000), (0x08100000, 0x0C0000, len(data) - 0xC0000)]

def off(a):
    for base, lo, size in IMAGES:
        if base <= a < base + size:
            return lo + a - base
    raise ValueError(hex(a))

def addr(o):
    for base, lo, size in IMAGES:
        if lo <= o < lo + size:
            return base + o - lo
    raise ValueError(hex(o))

def u32(a): return struct.unpack_from("<I", data, off(a))[0]

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS); md.skipdata = True
mdd = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS); mdd.detail = True; mdd.skipdata = True

def dis(a, n=40):
    o = off(a)
    return list(md.disasm(data[o:o + n * 4], a))[:n]

def slots(value):
    """Word-aligned addresses whose 32-bit content equals value."""
    pat = struct.pack("<I", value); out = []; i = data.find(pat)
    while i >= 0:
        if i % 4 == 0: out.append(addr(i))
        i = data.find(pat, i + 1)
    return out

def refs(slot, window=4096):
    """PC-relative loads/ADRs within `window` bytes before slot that resolve to it."""
    out = set()
    lo = max(off(slot) - window, 0)
    for start in (lo & ~1, (lo & ~1) + 2):
        for i in mdd.disasm(data[start:off(slot)], addr(start)):
            if i.id == 0: continue
            for op in i.operands:
                if op.type == ARM_OP_MEM and op.mem.base == ARM_REG_PC and ((i.address + 4) & ~3) + op.mem.disp == slot:
                    out.add((i.address, f"{i.mnemonic} {i.op_str}"))
    return sorted(out)

def func(a, back=0x2000):
    """Last 'push {..., lr}' at or before a (heuristic)."""
    best = None
    base = max(b for b, _, _ in IMAGES if b <= a)
    lo = max((a - back) & ~1, base)
    for start in (lo, lo + 2):
        for i in md.disasm(data[off(start):off(a) + 2], start):
            if i.mnemonic in ("push", "push.w", "stmdb") and "lr" in i.op_str and i.address <= a:
                if best is None or i.address > best: best = i.address
    return best

if __name__ == "__main__":
    cmd, *args = sys.argv[1:]
    if cmd == "dis":
        for i in dis(int(args[0], 16), int(args[1]) if len(args) > 1 else 40):
            print(f"{i.address:#010x}  {i.mnemonic:8s} {i.op_str}")
    elif cmd == "lit":
        for v in args:
            for s in slots(int(v, 16)):
                print(f"{int(v,16):#x} @ {s:#x}", refs(s))
    elif cmd == "str":
        t = args[0].encode(); i = data.find(t)
        while i >= 0:
            a = addr(i); print(f"'{args[0]}' @ {a:#x}")
            for s in slots(a): print(f"   slot {s:#x}", refs(s))
            i = data.find(t, i + 1)
    elif cmd == "func":
        f = func(int(args[0], 16)); print(hex(f) if f else None)
