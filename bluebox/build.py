"""Build a patched bluebox eurorack 1.5.1 image.

  python3 bluebox/build.py [features...]     default: loop      -> out/bluebox/<features>/BLUEEURO.BIN + build.json
  python3 bluebox/build.py --null            no features: must be byte-identical to stock (checked)

Every patch site is checked against its stock bytes first; every changed byte must lie in a registered range; every
BL written must land on a known symbol. The version string shown on the settings page marks the build ("1.5.1L1").
Stock image: firmware/BLUEEURO.BIN (SHA-256 below). Never commit or share the output.
"""
import hashlib
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "bluebox", "src")
OUT = os.path.join(ROOT, "out", "bluebox")
STOCK = os.path.join(ROOT, "firmware", "BLUEEURO.BIN")
STOCK_SHA256 = "fcb04565e2fd5bc15c0b2d0dc913dc99dc74732611e8cef0d7e8e4bb4af7402b"
BASE = 0x08040000                       # the file is contiguous flash from here (re/bluebox.md)
VERSION = (0x08149640, b"1.5.1\0\0\0", b"1.5.1L8\0")

# M7 cave: inside the zero-filled gap 0x0808AC00..0x08090000, below every cave bluelex uses (0x08090000..)
CAVE7 = (0x0808C000, 0x2000)
# M4 cave: the rest of that gap (the M4 executes from bank 1, as bluelex's M4 caves do)
CAVE4 = (0x0808E000, 0x2000)
COMMON = ["-mthumb", "-mfloat-abi=hard", "-O2", "-ffreestanding", "-nostdlib", "-fno-builtin", "-Wall", "-Wextra",
          "-Werror", "-Wl,-e,0"]
M7 = ["-mcpu=cortex-m7", "-mfpu=fpv5-d16"]
M4 = ["-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16"]


def bl(at, target):
    off = (target & ~1) - (at + 4)
    assert -(1 << 24) <= off < (1 << 24) and off % 2 == 0, f"BL out of range {at:#x}->{target:#x}"
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    return struct.pack("<HH", 0xF000 | (s << 10) | ((off >> 12) & 0x3FF),
                       0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF))


def compile_cave(name, sources, origin, length, cpu=M7):
    os.makedirs(OUT, exist_ok=True)
    ld = os.path.join(OUT, name + ".ld")
    open(ld, "w").write(
        f"MEMORY {{ CAVE (rx) : ORIGIN = {origin:#x}, LENGTH = {length:#x} }}\n"
        "SECTIONS {\n  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE\n"
        "  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }\n}\n")
    elf = os.path.join(OUT, name + ".elf")
    subprocess.run(["arm-none-eabi-gcc", *cpu, *COMMON, "-T", ld, "-o", elf, *[os.path.join(SRC, s) for s in sources]],
                   check=True)
    binf = os.path.join(OUT, name + ".bin")
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".cave", elf, binf], check=True)
    nm = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True, check=True).stdout
    syms = {n: int(a, 16) for a, k, n in (l.split() for l in nm.splitlines() if len(l.split()) == 3)}
    code = open(binf, "rb").read()
    assert len(code) <= length, f"{name}: {len(code)} bytes > cave {length}"
    return code, syms


def feature_loop():
    code, s = compile_cave("loop7", ["loop_m7.c"], *CAVE7)
    code4, s4 = compile_cave("loop4", ["loop_m4.c", "loop_ui_m4.c"], *CAVE4, cpu=M4)
    patches = [(CAVE7[0], bytes(len(code)), code, "loop7 cave"), (CAVE4[0], bytes(len(code4)), code4, "loop4 cave")]
    # channel node after the EQ, vtable 0x0806BF74, slot 3 (process 0x080574EC) -> loop_process (records the track's
    # output into the loop file, adds the loop)
    patches.append((0x0806BF80, struct.pack("<I", 0x080574ED), struct.pack("<I", s["loop_process"] | 1),
                    "channel process ptr = loop_process"))
    # M4: parameter table, last definition (id 0x117) -> loop_defs; channel set, add of id 0x2A -> loop_set_add
    patches.append((0x0813714A, bl(0x0813714A, 0x08135E08), bl(0x0813714A, s4["loop_defs"]), "def -> loop_defs"))
    patches.append((0x0812087C, bl(0x0812087C, 0x081205E2), bl(0x0812087C, s4["loop_set_add"]),
                    "set add -> loop_set_add"))
    # M4 Loop view (src/loop_ui_m4.c): Track page constructor, the key handler's showPage / setBlock, page messages
    patches.append((0x08135BA2, bl(0x08135BA2, 0x08133470), bl(0x08135BA2, s4["loop_track_ctor"]),
                    "track ctor -> loop_track_ctor"))
    patches.append((0x08135718, bl(0x08135718, 0x0813AA1C), bl(0x08135718, s4["loop_show"]), "showPage -> loop_show"))
    patches.append((0x08135720, bl(0x08135720, 0x08133884), bl(0x08135720, s4["loop_setblock"]),
                    "setBlock -> loop_setblock"))
    patches.append((0x0814E608, struct.pack("<I", 0x081342AD), struct.pack("<I", s4["loop_msg"] | 1),
                    "track page msg ptr = loop_msg"))
    # M4: UI loop, its redraw check (bl 0x0813AA9E @0x081351A6) -> loop_tick (loop files, state on the LOOP button)
    patches.append((0x081351A6, bl(0x081351A6, 0x0813AA9E), bl(0x081351A6, s4["loop_tick"]), "redraw -> loop_tick"))
    targets = {s4["loop_defs"] & ~1: "loop_defs",
               s4["loop_set_add"] & ~1: "loop_set_add", s4["loop_track_ctor"] & ~1: "loop_track_ctor",
               s4["loop_show"] & ~1: "loop_show", s4["loop_setblock"] & ~1: "loop_setblock",
               s4["loop_tick"] & ~1: "loop_tick"}
    return patches, targets, {"loop7": len(code), "loop7_free": CAVE7[1] - len(code), "loop4": len(code4),
                              "loop4_free": CAVE4[1] - len(code4)}


FEATURES = {"loop": feature_loop}


def main(args):
    null = "--null" in args
    names = [] if null else ([a for a in args if not a.startswith("-")] or ["loop"])
    data = bytearray(open(STOCK, "rb").read())
    assert hashlib.sha256(data).hexdigest() == STOCK_SHA256, "stock firmware hash mismatch"
    stock = bytes(data)
    patches, targets, sizes = [], {}, {}
    for n in names:
        p, t, sz = FEATURES[n]()
        patches += p
        targets.update(t)
        sizes.update(sz)
    if names:
        patches.append((VERSION[0], VERSION[1], VERSION[2], "version string"))
    ranges = []
    for addr, old, new, what in patches:
        o = addr - BASE
        assert len(old) == len(new), f"{what}: size change at {addr:#x}"
        assert data[o:o + len(old)] == old, f"{what}: stock bytes differ at {addr:#x}"
        data[o:o + len(new)] = new
        ranges.append((o, o + len(new)))
        print(f"  {addr:#010x} {len(new):6d}  {what}")
    # verification: every changed byte inside a registered range; BLs land on known symbols
    changed = [i for i in range(len(data)) if data[i] != stock[i]]
    stray = [i for i in changed if not any(lo <= i < hi for lo, hi in ranges)]
    assert not stray, f"bytes changed outside registered ranges: {[hex(BASE + i) for i in stray[:8]]}"
    for addr, _, new, what in patches:
        if " -> " in what:
            tgt = next(t for t, n in targets.items() if n == what.split(" -> ")[1])
            assert new == bl(addr, tgt), f"{what}: BL at {addr:#x} does not reach {tgt:#x}"
        if " ptr = " in what:
            ptr = struct.unpack("<I", new)[0] & ~1
            assert any(c[0] <= ptr < c[0] + c[1] for c in (CAVE7, CAVE4)), f"{what}: pointer outside the caves"
    if null:
        assert bytes(data) == stock, "null repack differs from stock"
        print("null repack: identical to stock")
        return
    d = os.path.join(OUT, "+".join(names))
    os.makedirs(d, exist_ok=True)
    out = os.path.join(d, "BLUEEURO.BIN")
    open(out, "wb").write(data)
    info = {"stock_sha256": STOCK_SHA256, "features": names, "version": VERSION[2].rstrip(b"\0").decode(),
            "sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "changed_bytes": len(changed),
            "cave_bytes": sizes}
    json.dump(info, open(os.path.join(d, "build.json"), "w"), indent=2)
    print(f"wrote {out}  sha256 {info['sha256'][:16]}  {len(changed)} bytes changed  {sizes}")


if __name__ == "__main__":
    main(sys.argv[1:])
