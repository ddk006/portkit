"""Unicorn test of the bluebox looper prototype (bluebox/src/loop_m7.c) on the built image.

  python3 bluebox/build.py loop && .venv/bin/python -P tests/test_bluebox_loop.py

The stock voice renderer FUN_08057A00 and fw_frames are replaced by a model of their behaviour (re/bluebox.md):
read n frames at pos, pos += n, left -= n, and when left < 1 the voice ends (active = 0). Checked:
  - a looping track keeps its voice alive and keeps  left == len - (pos - start)  (no drift), wrapping on time;
  - a track without its mask bit ends as stock;
  - a voice that is fading out (ending) is not looped.
"""
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, \
    UC_ARM_REG_LR, UC_ARM_REG_PC

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "out", "bluebox", "loop", "BLUEEURO.BIN")
BASE, SRAM4, STATE = 0x08040000, 0x38000000, 0x3800F000
STACK, RAM = 0x20010000, 0x24000000
PLAYER, VOICE, BUS = 0x24001000, 0x24001100, 0x24001200
FW_VOICE, FW_FRAMES, RET = 0x08057A00, 0x0804D4C0, 0x24000000
N = 32
VOICE_FMT = "<BBxxIq8xqq"                          # active, ending, asset, pos, delay, left


class Emu:
    def __init__(self):
        data = open(IMG, "rb").read()
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(BASE, (len(data) + 0xFFFF) & ~0xFFFF)
        self.uc.mem_write(BASE, data)
        self.uc.mem_map(SRAM4, 0x10000)
        self.uc.mem_map(0x20000000, 0x20000)
        self.uc.mem_map(RAM, 0x10000)
        self.uc.mem_write(RET, b"\x00\xbe")             # bkpt: end of a call
        self.uc.hook_add(UC_HOOK_CODE, self.stub, begin=FW_VOICE, end=FW_VOICE)
        self.uc.hook_add(UC_HOOK_CODE, self.stub, begin=FW_FRAMES, end=FW_FRAMES)
        self.sym = self.symbol("loop_voice")
        self.reads = []

    @staticmethod
    def symbol(name):
        import subprocess
        out = subprocess.run(["arm-none-eabi-nm", os.path.join(ROOT, "out", "bluebox", "loop7.elf")],
                             capture_output=True, text=True, check=True).stdout
        return next(int(l.split()[0], 16) for l in out.splitlines() if l.split()[-1] == name)

    def voice(self, **kw):
        cur = dict(zip(("active", "ending", "asset", "pos", "delay", "left"),
                       struct.unpack(VOICE_FMT, bytes(self.uc.mem_read(VOICE, 0x28)))))
        if kw:
            cur.update(kw)
            self.uc.mem_write(VOICE, struct.pack(VOICE_FMT, *(cur[k] for k in
                              ("active", "ending", "asset", "pos", "delay", "left"))))
        return cur

    def stub(self, uc, addr, size, _):
        if addr == FW_FRAMES:
            uc.reg_write(UC_ARM_REG_R0, N)
        else:                                          # stock voice model
            v = self.voice()
            if v["active"]:
                self.reads.append(v["pos"])
                v["pos"] += N
                if not v["ending"]:
                    v["left"] -= N
                    if v["left"] < 1:
                        v["active"] = 0
                self.voice(**v)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def block(self):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, STACK)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        for r, v in ((UC_ARM_REG_R0, PLAYER), (UC_ARM_REG_R1, VOICE), (UC_ARM_REG_R2, BUS), (UC_ARM_REG_R3, 0)):
            uc.reg_write(r, v)
        uc.emu_start(self.sym | 1, RET, count=20000)

    def state(self):
        magic, mask, wraps = struct.unpack("<III", bytes(self.uc.mem_read(STATE, 12)))
        return magic, mask, wraps


def play(track, length, blocks, start=5000, ending=0, mask=None):
    e = Emu()
    e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", track))
    e.voice(active=1, ending=ending, asset=7, pos=start, delay=0, left=length)
    e.block()                                         # first block initialises the state (mask = all)
    if mask is not None:
        e.uc.mem_write(STATE + 4, struct.pack("<I", mask))
    for _ in range(blocks - 1):
        if not e.voice()["active"]:
            break
        e.block()
    return e


def main():
    fails = 0

    def check(ok, what):
        nonlocal fails
        print(("ok   " if ok else "FAIL ") + what)
        fails += not ok

    L, B = 1000, 400                                   # clip of 1000 samples, 400 blocks of 32 = 12.8 passes
    e = play(track=0, length=L, blocks=B)
    v = e.voice()
    magic, mask, wraps = e.state()
    check(v["active"] == 1, "looping voice stays active")
    check(v["left"] == L - (v["pos"] - 5000), "left == len - (pos - start) after %d wraps" % wraps)
    check(wraps == (B * N) // L, "wraps on time: %d (expected %d)" % (wraps, (B * N) // L))
    check(all(5000 <= p < 5000 + L for p in e.reads), "every read starts inside the clip")
    check(max(e.reads) + N - (5000 + L) < N, "overrun past the clip end < one block")

    e = play(track=3, length=L, blocks=B, mask=0x1)    # track 3 not in the mask -> stock end
    check(e.voice()["active"] == 0 and e.state()[2] == 0, "track without its mask bit ends as stock")

    e = play(track=0, length=L, blocks=B, ending=1)    # fading voice: not looped, never touched
    check(e.state()[2] == 0, "fading voice is not looped")

    e = play(track=0, length=20, blocks=10)            # clip shorter than a block: left alone (stock end)
    check(e.voice()["active"] == 0, "clip shorter than a block ends as stock")
    print("PASS" if not fails else f"{fails} FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
