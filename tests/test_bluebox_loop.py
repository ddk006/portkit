"""Unicorn test of the bluebox looper prototype (bluebox/src/loop_m7.c) on the built image.

  python3 bluebox/build.py loop && .venv/bin/python -P tests/test_bluebox_loop.py

The stock voice renderer FUN_08057A00 and fw_frames are replaced by a model of their behaviour (re/bluebox.md):
read n frames at pos, pos += n, left -= n, and when left < 1 the voice ends (active = 0). Checked:
  - a looping track keeps its voice alive and keeps  left == len - (pos - start)  (no drift), wrapping on time;
  - a track without its mask bit ends as stock;
  - a voice that is fading out (ending) is not looped.
Crossfade (loop_read, replacing the cache read; the stock read FUN_0805B350 is modelled as a 440 Hz sine "file"):
  - every block is rendered as the voice does (loop_voice, then a read at the voice's position): the output stays
    smooth across the jumps (second difference within a few times that of the sine), with and without audio past the
    loop end in the file;
  - reads outside the crossfade zone, before the first jump and of other assets pass through unchanged.
Bars (transport modelled: a3 -> pointer -> object with float bpm +0x18, int64 ticks +0x30, advancing with the blocks):
  - the loop starts on the first bar line after the clip's start and is exactly `bars` bars long;
  - at a tempo where a bar is not a whole number of samples the jumps alternate and never drift;
  - a clip too short for `bars` loops fewer bars, one shorter than a bar loops whole.
Tracks and slots (loop_process with the stock queue / event / player modelled; M4 loop_defs / loop_set_add):
  - LOOP takes one of two slots in switch-on order; a third track waits and gets the next free slot;
  - BARS is kept per track and clamped to 1..16; events for another queue are ignored; the stock process always runs;
  - LOOP off ends a looping clip within a block;
  - the M4 hooks call the stock function, then define / add LOOP, BARS, FILTER and FADE.
Filter and fade (FILTER / FADE per track; the voice's read is modelled right after loop_voice, as it happens inside):
  - FILTER < 0 low-passes (high tone down, low tone through), > 0 high-passes, 0 leaves the signal untouched;
  - LOOP off with FADE = 1 bar: the level follows a smooth fade to zero over one bar, then the clip ends and the
    slot is free; LOOP on during the fade cancels it.
"""
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, \
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_CPU_ARM_MAX

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "out", "bluebox", "loop", "BLUEEURO.BIN")
BASE, SRAM4, STATE = 0x08040000, 0x38000000, 0x3800F000
STACK, RAM = 0x20010000, 0x24000000
PLAYER, VOICE, BUS = 0x24001000, 0x24001100, 0x24001200
FW_VOICE, FW_FRAMES, FW_READ, RET = 0x08057A00, 0x0804D4C0, 0x0805B350, 0x24000000
BUFL, BUFR, BUFT = 0x24002000, 0x24002400, 0x24002800
TRANSPORT, A3 = 0x24003000, 0x24003100
FW_QUEUE, FW_NEXT_EV, FW_PLAYER, QUEUE = 0x08053C8C, 0x0804E938, 0x080574EC, 0x24004000
FW_DEF, FW_ADD = 0x08135E08, 0x081205E2
FW_PLAN, PL2, FW_PLAYER_PROC = 0x08057900, 0x24005000, 0x08057B70
OWNER, CHBARS, WANT = STATE + 0x710, STATE + 0x712, STATE + 0x71E
CHFILTER, CHFADE = STATE + 0x720, STATE + 0x738
W = 2 * 3.141592653589793 * 440 / 48000
N = 32
VOICE_FMT = "<BBxxIq8xqq"                          # active, ending, asset, pos, delay, left


class Emu:
    def __init__(self):
        data = open(IMG, "rb").read()
        # as tests/bb_emu.py: an A-profile CPU in Thumb state with the FPU on runs the Thumb-2 / FPv5 code
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_MAX)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.uc.mem_map(BASE, (len(data) + 0xFFFF) & ~0xFFFF)
        self.uc.mem_write(BASE, data)
        self.uc.mem_map(SRAM4, 0x10000)
        self.uc.mem_map(0x20000000, 0x20000)
        self.uc.mem_map(RAM, 0x10000)
        self.uc.mem_write(RET, b"\x00\xbe")             # bkpt: end of a call
        self.uc.hook_add(UC_HOOK_CODE, self.stub, begin=FW_VOICE, end=FW_VOICE)
        self.uc.hook_add(UC_HOOK_CODE, self.stub, begin=FW_FRAMES, end=FW_FRAMES)
        self.uc.hook_add(UC_HOOK_CODE, self.read_stub, begin=FW_READ, end=FW_READ)
        self.sym = self.symbol("loop_voice")
        self.sym_read = self.symbol("loop_read")
        self.sym_proc = self.symbol("loop_process")
        self.events, self.player_calls, self.m4_calls, self.vstarts = [], 0, [], 0
        self.vstart_calls, self.fail_calls = 0, 0
        for a in (FW_QUEUE, FW_NEXT_EV, FW_PLAYER, FW_DEF, FW_ADD, FW_PLAN, FW_PLAYER_PROC):
            self.uc.hook_add(UC_HOOK_CODE, self.api_stub, begin=a, end=a)
        self.file_len = 1 << 40
        self.a3 = 0
        self.reads = []

    @staticmethod
    def symbol(name, elf="loop7.elf"):
        import subprocess
        out = subprocess.run(["arm-none-eabi-nm", os.path.join(ROOT, "out", "bluebox", elf)],
                             capture_output=True, text=True, check=True).stdout
        return next(int(l.split()[0], 16) for l in out.splitlines() if l.split()[-1] == name)

    def api_stub(self, uc, addr, size, _):
        r = lambda k: uc.reg_read(k)
        if addr == FW_QUEUE:
            uc.reg_write(UC_ARM_REG_R0, QUEUE)
        elif addr == FW_NEXT_EV:                        # (queue, index, event*) -> 1 and the event, or 0
            i, dst = r(UC_ARM_REG_R1), r(UC_ARM_REG_R2)
            if i < len(self.events):
                dest, pid, val = self.events[i]
                uc.mem_write(dst, struct.pack("<B7xIHxxiI", 0x39, dest, pid, val, 0))
                uc.reg_write(UC_ARM_REG_R0, 1)
            else:
                uc.reg_write(UC_ARM_REG_R0, 0)
        elif addr == FW_PLAN:                            # stock planning + voice start, as read (re/bluebox.md)
            self.vstart_calls += 1
            player, a3 = r(UC_ARM_REG_R0), r(UC_ARM_REG_R2)
            idx = struct.unpack("<i", bytes(uc.mem_read(player + 0x5A0, 4)))[0]
            clip = player + 0x98 + 0x28 * idx
            _, _, asset, _, cstart, cend, coff = struct.unpack("<IIIIqqq", bytes(uc.mem_read(clip, 0x28)))
            t = struct.unpack("<I", bytes(uc.mem_read(a3, 4)))[0]
            bpm = struct.unpack("<f", bytes(uc.mem_read(t + 0x18, 4)))[0]
            now = struct.unpack("<q", bytes(uc.mem_read(t + 0x30, 8)))[0]
            smp = lambda tk: int(tk / 960 / bpm * 60 * 48000 + 0.5)
            if self.vstart_calls > self.fail_calls:     # file length known to the cache by then
                if now < cstart:                        # coming clip: voice waits (delay), then plays from offset
                    pos, left, delay = smp(coff), smp(cend) - smp(cstart), smp(cstart) - smp(now)
                else:                                   # mid-clip
                    pos, left, delay = smp(coff + now - cstart), smp(cend) - smp(now), 0
                if left > 0:
                    va = struct.unpack("<B", bytes(uc.mem_read(player + 0x20, 1)))[0]
                    v = player + (0x58 if va else 0x20)
                    uc.mem_write(v, struct.pack(VOICE_FMT, 1, 0, asset, pos, delay, left))
                    self.vstarts += 1
        elif addr == FW_PLAYER_PROC:
            uc.reg_write(UC_ARM_REG_R0, 0)
        elif addr == FW_PLAYER:
            self.player_calls += 1
            uc.reg_write(UC_ARM_REG_R0, 77)
        else:                                           # M4 def / add: record (id, value or type)
            self.m4_calls.append(("def" if addr == FW_DEF else "add", r(UC_ARM_REG_R1), r(UC_ARM_REG_R2)))
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def call(self, sym, *args, stack=b""):
        uc = self.uc
        sp = STACK - 64
        uc.mem_write(sp, stack + b"\0" * (64 - len(stack)))
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(reg, v)
        uc.emu_start(sym | 1, RET, count=200000)
        return uc.reg_read(UC_ARM_REG_R0)

    def process(self, track, events):
        """loop_process for the player node of `track` with these (dest, id, value) events in its queue"""
        self.uc.mem_write(PLAYER + 0x18, struct.pack("<I", track))
        self.events = events
        return self.call(self.sym_proc, PLAYER, 0)

    def init(self, mask=0, bars=0):
        """let the code initialise its state (as at power-up), then set the debug mask / global bars"""
        self.uc.mem_write(STATE, b"\0" * 0x900)
        self.process(15, [])                            # state() initialises; track 15 has no queue
        self.uc.mem_write(STATE + 4, struct.pack("<I", mask))
        self.uc.mem_write(STATE + 12, struct.pack("<I", bars))

    def owners(self):
        o = bytes(self.uc.mem_read(OWNER, 2))
        return [b - 1 if b else None for b in o]

    def voice(self, at=VOICE, **kw):
        cur = dict(zip(("active", "ending", "asset", "pos", "delay", "left"),
                       struct.unpack(VOICE_FMT, bytes(self.uc.mem_read(at, 0x28)))))
        if kw:
            cur.update(kw)
            self.uc.mem_write(at, struct.pack(VOICE_FMT, *(cur[k] for k in
                              ("active", "ending", "asset", "pos", "delay", "left"))))
        return cur

    def stub(self, uc, addr, size, _):
        if addr == FW_FRAMES:
            uc.reg_write(UC_ARM_REG_R0, N)
        else:                                          # stock voice model, on the voice passed in r1
            at = uc.reg_read(UC_ARM_REG_R1)
            v = self.voice(at)
            if v["active"] and v["delay"] > 0:
                v["delay"] = max(0, v["delay"] - N)
                self.voice(at, **v)
            elif v["active"]:
                self.reads.append(v["pos"])
                v["pos"] += N
                if not v["ending"]:
                    v["left"] -= N
                    if v["left"] < 1:
                        v["active"] = 0
                self.voice(at, **v)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def read_stub(self, uc, addr, size, _):
        """stock read: x(p) = 0.5 sin(W p) for 0 <= p < file_len, both channels; 0 = failure past the end"""
        import math
        pos = (uc.reg_read(UC_ARM_REG_R3) << 32) | uc.reg_read(UC_ARM_REG_R2)
        sp = uc.reg_read(UC_ARM_REG_SP)
        asset, l, r, n = struct.unpack("<IIII", bytes(uc.mem_read(sp, 16)))
        ok = pos + n <= self.file_len
        if ok:
            fn = getattr(self, "read_stub_fn", None) or (lambda p: 0.5 * math.sin(W * p))
            vals = struct.pack(f"<{n}f", *(fn(pos + i) for i in range(n)))
            for buf in (l, r):
                if buf:
                    uc.mem_write(buf, vals)
        uc.reg_write(UC_ARM_REG_R0, 1 if ok else 0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def read(self, pos, n, asset=7):
        """loop_read(cache, fn, pos, asset, l, r, n) -> (ok, left samples)"""
        uc = self.uc
        uc.mem_write(BUFL, b"\0" * 4 * n)
        sp = STACK - 16
        uc.mem_write(sp, struct.pack("<IIII", asset, BUFL, BUFR, n))
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        for reg, v in ((UC_ARM_REG_R0, 0), (UC_ARM_REG_R1, 0), (UC_ARM_REG_R2, pos & 0xFFFFFFFF),
                       (UC_ARM_REG_R3, pos >> 32)):
            uc.reg_write(reg, v)
        uc.emu_start(self.sym_read | 1, RET, count=200000)
        ok = uc.reg_read(UC_ARM_REG_R0)
        vals = list(struct.unpack(f"<{n}f", bytes(uc.mem_read(BUFL, 4 * n))))
        if not ok:
            vals = [0.0] * n                            # the voice zero-fills on a failed read
        return ok, vals

    def block(self):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, STACK)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        for r, v in ((UC_ARM_REG_R0, PLAYER), (UC_ARM_REG_R1, VOICE), (UC_ARM_REG_R2, BUS), (UC_ARM_REG_R3, self.a3)):
            uc.reg_write(r, v)
        uc.emu_start(self.sym | 1, RET, count=20000)

    def state(self):
        magic, mask, wraps = struct.unpack("<III", bytes(self.uc.mem_read(STATE, 12)))
        return magic, mask, wraps


def play(track, length, blocks, start=5000, ending=0, mask=None):
    e = Emu()
    e.init(mask=0xFFF)                                                # debug mask: every track loops
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
    import math

    def render(file_extra, L=3000, B=400, start=5000):
        """the voice's view: per block, its read position before loop_voice, then the read there"""
        e = Emu()
        e.init(mask=0xFFF)
        e.file_len = start + L + file_extra
        e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", 0))
        e.voice(active=1, ending=0, asset=7, pos=start, delay=0, left=L)
        out = []
        for _ in range(B):
            p = e.voice()["pos"]
            e.block()
            out += e.read(p, N)[1]
        return e, out

    lim = 0.5 * W * W
    e, out = render(1000)
    d2 = max(abs(out[i + 1] - 2 * out[i] + out[i - 1]) for i in range(1, len(out) - 1))
    check(d2 < 4 * lim, "crossfade, audio past the loop end: max 2nd diff %.4f (sine %.4f), %d jumps"
          % (d2, lim, e.state()[2]))
    e, out = render(0)                                 # file ends at the loop end: no silent block before a jump
    runs, run = [], 0
    for v in out[200:]:
        run = run + 1 if v == 0.0 else 0
        runs.append(run)
    check(max(runs) < N, "file ends at the loop end: longest silence %d samples (< one block), %d jumps"
          % (max(runs), e.state()[2]))
    e, out = render(1000, B=60)                        # first pass only (2 * 32 * 60 < 3000 + start offset): untouched
    check(all(abs(out[i] - 0.5 * math.sin(W * (5000 + i))) < 1e-5 for i in range(len(out))), "no blend before the first jump")
    e, _ = render(1000)
    ok, vals = e.read(5000 + 10, N, asset=9)           # another asset in the zone: pass through
    check(all(abs(v - 0.5 * math.sin(W * (5010 + i))) < 1e-5 for i, v in enumerate(vals)), "other assets pass through")
    ok, vals = e.read(5000 + 1000, N)                  # same asset outside the zone: pass through
    check(all(abs(v - 0.5 * math.sin(W * (6000 + i))) < 1e-5 for i, v in enumerate(vals)), "reads outside the zone pass through")
    def bars_run(bpm, bars, t0, clip, passes):
        """voice at file pos 0 starting at tick t0; returns (jumps [(size, landing)], loop start expected)"""
        e = Emu()
        e.a3 = A3
        e.uc.mem_write(A3, struct.pack("<I", TRANSPORT))
        e.init(mask=0xFFF, bars=bars)
        e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", 0))
        e.voice(active=1, ending=0, asset=7, pos=0, delay=0, left=clip)
        spt = 48000 * 60 / (960 * bpm)
        ticks, jumps = float(t0), []
        while len(jumps) < passes and e.voice()["active"]:
            e.uc.mem_write(TRANSPORT + 0x18, struct.pack("<f", bpm))
            e.uc.mem_write(TRANSPORT + 0x30, struct.pack("<q", int(ticks)))
            p = e.voice()["pos"]
            e.block()
            q = e.voice()["pos"]
            if q < p:
                jumps.append((p + N - q, q))
            ticks += N / spt
            if ticks > t0 + 200 * 3840:
                break
        bar = -(-t0 // 3840) * 3840
        return jumps, round((bar - t0) * spt), e.voice()["active"]

    jumps, ls, _ = bars_run(120, 2, 1251, 6 * 96000, 3)
    check(len(jumps) == 3 and all(sz == 192000 for sz, _ in jumps), "120 BPM, 2 bars: jumps %s" % [j[0] for j in jumps])
    check(all(ls <= at < ls + N for _, at in jumps), "120 BPM: lands on the bar line at sample %d: %s" % (ls, [j[1] for j in jumps]))
    jumps, ls, _ = bars_run(133, 3, 0, 4 * 259850, 6)
    exact = 3 * 3840 * 48000 * 60 / (960 * 133)
    total = sum(sz for sz, _ in jumps)
    check(len(jumps) == 6 and abs(total - 6 * exact) < 1, "133 BPM, 3 bars: jumps %s, sum %d vs %.2f"
          % (sorted(set(j[0] for j in jumps)), total, 6 * exact))
    jumps, ls, _ = bars_run(120, 4, 0, int(1.5 * 96000), 2)
    check(len(jumps) == 2 and all(sz == 96000 for sz, _ in jumps), "clip of 1.5 bars, 4 asked: loops 1 bar (%s)" % [j[0] for j in jumps])
    jumps, ls, _ = bars_run(120, 4, 0, 50000, 2)
    check(len(jumps) == 2 and all(sz == 50000 for sz, _ in jumps), "clip shorter than a bar: loops whole (%s)" % [j[0] for j in jumps])
    # --- tracks and slots
    e = Emu()
    e.init()
    P_LOOP, P_BARS = 0x103, 0x104
    calls0 = e.player_calls
    ret = e.process(0, [(0, P_LOOP, 1)])
    e.process(1, [(1, P_LOOP, 1)])
    e.process(2, [(2, P_LOOP, 1)])
    check(e.owners() == [0, 1], "two slots in switch-on order: %s" % e.owners())
    check(ret == 77 and e.player_calls - calls0 == 3, "stock player process runs and its result is returned")
    e.process(0, [(0, P_LOOP, 0)])
    check(e.owners() == [2, 1], "track 0 off: waiting track 2 gets its slot: %s" % e.owners())
    e.process(1, [(1, P_BARS, 8), (5, P_BARS, 3)])
    e.process(3, [(3, P_BARS, 99)])
    e.process(4, [(4, P_BARS, 0)])
    cb = bytes(e.uc.mem_read(CHBARS, 12))
    check(cb[1] == 8 and cb[3] == 16 and cb[4] == 1 and cb[5] == 0, "BARS per track, clamped, other queues ignored: %s" % list(cb[:6]))

    # LOOP off ends a looping clip
    e = Emu()
    e.init()
    e.process(0, [(0, P_LOOP, 1)])
    e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", 0))
    e.voice(active=1, ending=0, asset=7, pos=5000, delay=0, left=1000)
    for _ in range(100):
        e.block()
    looped = e.state()[2] > 0 and e.voice()["active"]
    e.process(0, [(0, P_LOOP, 0)])
    e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", 0))
    for _ in range(2):
        if e.voice()["active"]:
            e.block()
    check(looped and not e.voice()["active"], "LOOP off ends the looping clip within a block")

    # M4 hooks
    m4 = Emu()
    for name, args, stack in (("loop_defs", (0x30000000, 0x117, 4, 0x08149000), struct.pack("<iiI", 0, 1, 0x08149010)),
                              ("loop_set_add", (0x30000100, 0x2A, 500), b"")):
        m4.call(Emu.symbol(name, "loop4.elf"), *args, stack=stack)
    calls = [(k, hex(i)) for k, i, _ in m4.m4_calls]
    check(calls == [("def", "0x117"), ("def", "0x103"), ("def", "0x104"), ("def", "0x105"), ("def", "0x107"),
                    ("add", "0x2a"), ("add", "0x103"), ("add", "0x104"), ("add", "0x105"), ("add", "0x107")],
          "M4: stock call first, then LOOP, BARS, FILTER, FADE: %s" % calls)
    # M4: a REC with a LOOP track ends the take BARS bars after the next bar line, the transport is not touched
    def rec_run(params, rec_at, ticks_end=40000, step=24, take_flag_after=10):
        e = Emu()
        uc = e.uc
        uc.mem_map(0x30000000, 0x50000)
        calls = []
        ticks = [rec_at]
        errs = [None]
        ERRS = 0x30044270 + 0x362C
        uc.mem_write(ERRS, struct.pack("<I", 3))
        def m4stub(uc, addr, size, _):
            r = lambda k: uc.reg_read(k)
            if addr == 0x08122580:                      # get(store, &set, &id, &val)
                st = struct.unpack("<H", bytes(uc.mem_read(r(UC_ARM_REG_R1), 2)))[0]
                pid = struct.unpack("<H", bytes(uc.mem_read(r(UC_ARM_REG_R2), 2)))[0]
                uc.mem_write(r(UC_ARM_REG_R3), struct.pack("<i", params.get((st, pid), 0)))
            elif addr == 0x081272CC:                    # record on/off
                calls.append(("setrec", r(UC_ARM_REG_R1), ticks[0]))
                uc.mem_write(0x30018010 + 0x743B, b"\0")
                errs[0] = ticks[0]                      # the Mixer's misses follow (see loop_ui_m4.c)
            elif addr == 0x08126524:
                calls.append(("transport", 0, ticks[0]))
            elif addr == 0x08126E8C:                    # store message: 0x6E param set
                code, tr, pid, val = struct.unpack("<H6xHxxIi", bytes(uc.mem_read(r(UC_ARM_REG_R1), 20)))
                calls.append(("set", (code, tr, pid, val), ticks[0]))
            else:                                       # page manager message / redraw check
                uc.reg_write(UC_ARM_REG_R0, 0)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        for adr in (0x08122580, 0x081272CC, 0x08126524, 0x08126E8C, 0x0813AA90, 0x0813AA9E):
            uc.hook_add(UC_HOOK_CODE, m4stub, begin=adr, end=adr)
        uc.mem_write(0x300478B0, struct.pack("<i", rec_at))
        uc.mem_write(STACK - 0x100, struct.pack("<H", 0x69))
        e.call(Emu.symbol("loop_app_msg", "loop4.elf"), 0x30010000, STACK - 0x100)
        n = 0
        while ticks[0] < ticks_end:
            n += 1
            if n == take_flag_after:
                uc.mem_write(0x30018010 + 0x743B, b"\1")   # the take runs once the recorder started
            uc.mem_write(0x300478B0, struct.pack("<i", ticks[0]))
            e.call(Emu.symbol("loop_tick", "loop4.elf"), 0x30010000, 0)
            if errs[0] is not None and ticks[0] - errs[0] in (96, 120, 144):     # misses right after the take
                uc.mem_write(ERRS, struct.pack("<I", struct.unpack("<I", bytes(uc.mem_read(ERRS, 4)))[0] + 2))
            if ticks[0] == rec_at + 30000 // step * step:                       # a real error much later
                uc.mem_write(ERRS, struct.pack("<I", struct.unpack("<I", bytes(uc.mem_read(ERRS, 4)))[0] + 1))
            ticks[0] += step
        rec_run.errs = struct.unpack("<I", bytes(uc.mem_read(ERRS, 4)))[0]
        return calls
    c = rec_run({(0, 0x162): 1, (0, 0x103): 1, (0, 0x104): 2, (3, 0x162): 1, (3, 0x103): 1, (5, 0x162): 1}, rec_at=1615)
    check(len(c) == 3 and c[0][:2] == ("setrec", 0) and 0 <= c[0][2] - (3840 + 2 * 3840 + 240) < 24,
          "REC with LOOP (BARS 2) at tick 1615: record off once at bar 1 + 2 bars + a 16th, no transport change: %s" % c)
    check([x[1] for x in c[1:]] == [(0x6E, 0, 0x162, 0), (0x6E, 3, 0x162, 0)],
          "then Rec Enable off for the loop tracks only (0, 3; not 5 without LOOP): %s" % c[1:])
    check(rec_run.errs == 4, "stream errors right after the take are counted back, later ones stay: %d (want 3 + 1)"
          % rec_run.errs)
    c = rec_run({(0, 0x162): 1, (0, 0x103): 1, (0, 0x104): 1}, rec_at=3700)
    check(len(c) == 2 and 0 <= c[0][2] - (7680 + 3840 + 240) < 24,
          "REC less than a 16th before a bar line: the loop starts on the bar after: %s" % c)
    c = rec_run({(0, 0x162): 1, (0, 0x103): 0, (0, 0x104): 2}, rec_at=1615)
    check(c == [], "REC without LOOP: the stock recording, nothing switched off: %s" % c)
    # --- filter and fade
    def tone_run(freq, filt=0, fade=0, off_at=None, on_again_at=None, blocks=600, bpm=120):
        """track 0 looping a sine `freq` file (BARS 0: whole 30000-sample clip); returns output samples, emu"""
        e = Emu()
        e.init()
        e.a3 = A3
        e.uc.mem_write(A3, struct.pack("<I", TRANSPORT))
        e.uc.mem_write(TRANSPORT + 0x18, struct.pack("<f", bpm))
        e.process(0, [(0, P_LOOP, 1), (0, 0x105, filt), (0, 0x107, fade)])
        w = 2 * math.pi * freq / 48000
        e.read_stub_fn = lambda p: 0.5 * math.sin(w * p)
        e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", 0))
        e.voice(active=1, ending=0, asset=7, pos=0, delay=0, left=30000)
        out = []
        for b in range(blocks):
            if b == off_at:
                e.process(0, [(0, P_LOOP, 0)])
            if b == on_again_at:
                e.process(0, [(0, P_LOOP, 1)])
            e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", 0))
            if not e.voice()["active"]:
                out += [0.0] * N
                continue
            p = e.voice()["pos"]
            e.block()
            out += e.read(p, N)[1]
        return out, e

    def rms(x):
        return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0

    hi_lp, _ = tone_run(5000, filt=-800, blocks=300)
    lo_lp, _ = tone_run(100, filt=-800, blocks=300)
    hi_hp, _ = tone_run(5000, filt=800, blocks=300)
    lo_hp, _ = tone_run(100, filt=800, blocks=300)
    flat, _ = tone_run(1000, filt=0, blocks=300)
    tail = slice(4800, None)
    check(rms(hi_lp[tail]) < 0.01 and rms(lo_lp[tail]) > 0.3, "FILTER -800 low-pass: 5 kHz %.4f, 100 Hz %.3f"
          % (rms(hi_lp[tail]), rms(lo_lp[tail])))
    check(rms(lo_hp[tail]) < 0.03 and rms(hi_hp[tail]) > 0.3, "FILTER +800 high-pass: 100 Hz %.4f, 5 kHz %.3f"
          % (rms(lo_hp[tail]), rms(hi_hp[tail])))
    check(all(abs(v - 0.5 * math.sin(2 * math.pi * 1000 / 48000 * (i % 30000))) < 1e-4 for i, v in enumerate(flat[:20000])),
          "FILTER 0: signal untouched")
    bar = 96000 // N                                   # one bar at 120 BPM, in blocks
    out, e = tone_run(1000, fade=1, off_at=100, blocks=100 + bar + 20)
    seg = lambda b0, b1: rms(out[b0 * N:b1 * N])
    lv = [seg(100 + bar * k // 4, 100 + bar * k // 4 + 50) for k in range(4)]
    after = seg(100 + bar + 5, 100 + bar + 20)
    check(lv[0] > lv[1] > lv[2] > lv[3] > after and after < 1e-4 and not e.voice()["active"],
          "FADE 1 bar: level %s, after %.5f, clip ended" % (["%.3f" % x for x in lv], after))
    check(e.owners() == [None, None], "slot free after the fade: %s" % e.owners())
    out, e = tone_run(1000, fade=1, off_at=100, on_again_at=100 + bar // 2, blocks=100 + bar + 200)
    check(e.voice()["active"] and rms(out[-50 * N:]) > 0.3 and e.owners()[0] == 0, "LOOP on during the fade cancels it")
    e = Emu()                                          # LOOP off with FADE while nothing plays: slot freed at once
    e.init()
    e.process(0, [(0, P_LOOP, 1), (0, 0x107, 4)])
    e.process(0, [(0, P_LOOP, 0)])
    e.process(0, [])
    check(e.owners() == [None, None], "LOOP off with FADE and no voice playing frees the slot: %s" % e.owners())
    # --- autostart of a freshly recorded clip
    def auto_run(loop_on=True, blocks=6000, bpm=120.0, bars=2, cstart=1537, extra=240, after=100, fail_calls=0):
        e = Emu()
        e.init()
        e.fail_calls = fail_calls
        e.a3 = A3
        e.uc.mem_write(A3, struct.pack("<I", TRANSPORT))
        e.uc.mem_write(TRANSPORT + 0x18, struct.pack("<f", bpm))
        ls = -(-cstart // 3840) * 3840
        cend = ls + bars * 3840 + extra
        e.uc.mem_write(PL2, b"\0" * 0x600)
        e.uc.mem_write(PL2 + 0x18, struct.pack("<I", 0))
        e.uc.mem_write(PL2 + 0x98, struct.pack("<IIIIqqq", 0, 0, 5, 0, cstart, cend, 0))
        e.uc.mem_write(PL2 + 0x598, struct.pack("<i", 1))
        e.uc.mem_write(PLAYER + 0x18, struct.pack("<I", 0))
        if loop_on:
            e.process(0, [(0, 0x103, 1), (0, 0x104, bars)])
        spt = 48000 * 60 / (960 * bpm)
        ticks = float(cend + after)
        log = []
        sym_pl = Emu.symbol("loop_player")
        for b in range(blocks):
            e.uc.mem_write(TRANSPORT + 0x30, struct.pack("<q", int(ticks)))
            act = (e.uc.mem_read(PL2 + 0x20, 1)[0], e.uc.mem_read(PL2 + 0x58, 1)[0])
            e.call(sym_pl, PL2, A3)                     # the player process first (autostart), then its voices
            for voff, a0 in zip((0x20, 0x58), act):
                if not a0 and e.uc.mem_read(PL2 + voff, 1)[0]:
                    log.append(("start", ticks, struct.unpack("<q", bytes(e.uc.mem_read(PL2 + voff + 8, 8)))[0]))
            for voff in (0x20, 0x58):                   # the stock order: voice A, then voice B
                pv = PL2 + voff
                before = struct.unpack("<q", bytes(e.uc.mem_read(pv + 8, 8)))[0]
                active_before = e.uc.mem_read(pv, 1)[0]
                uc = e.uc
                uc.reg_write(UC_ARM_REG_SP, STACK)
                uc.reg_write(UC_ARM_REG_LR, RET | 1)
                for reg, val in ((UC_ARM_REG_R0, PL2), (UC_ARM_REG_R1, pv), (UC_ARM_REG_R2, BUS), (UC_ARM_REG_R3, A3)):
                    uc.reg_write(reg, val)
                saved = (VOICE,)
                uc.emu_start(e.sym | 1, RET, count=40000)
                after_pos = struct.unpack("<q", bytes(e.uc.mem_read(pv + 8, 8)))[0]
                if active_before and after_pos < before:
                    log.append(("jump", ticks, after_pos))
                if not active_before and e.uc.mem_read(pv, 1)[0]:
                    log.append(("start", ticks, after_pos))
            ticks += N / spt
        return e, log, ls, cend

    e, log, ls, cend = auto_run(blocks=16000)
    starts = [x for x in log if x[0] == "start"]
    jumps = [x for x in log if x[0] == "jump"]
    spt = 25.0
    want_pos = round(((cend + 100 + 240 - ls) % 7680 + ls - 1537) * spt)   # phase a 16th after the take ends
    check(len(starts) == 1 and abs(starts[0][2] - want_pos) <= 1 + 32 * 2,
          "autostart: one start at file pos %s (want ~%d)" % ([x[2] for x in starts], want_pos))
    grid = [ls + k * 7680 for k in range(1, 10)]
    offs = [min(abs(j[1] - g) for g in grid) for j in jumps]
    check(len(jumps) >= 2 and max(offs) < 3, "autostart: jumps on the bar grid (off by %s ticks), %d jumps"
          % (["%.1f" % o for o in offs], len(jumps)))
    check(e.vstarts == 1, "autostart only once per clip (%d starts)" % e.vstarts)
    e3, log3, ls3, cend3 = auto_run(blocks=300, fail_calls=5)
    st3 = [x for x in log3 if x[0] == "start"]
    check(e3.vstarts == 1 and e3.vstart_calls == 6 and len(st3) == 1,
          "autostart retries until the stock start starts the voice (%d calls, %d starts)" % (e3.vstart_calls, e3.vstarts))
    e4, log4, _, _ = auto_run(blocks=7000, bpm=60.0, bars=1)
    check(e4.vstarts == 1 and e4.vstart_calls == 1 and any(x[0] == "jump" for x in log4),
          "autostart at 60 BPM: the coming clip waits as a delayed voice, then loops (%d calls)" % e4.vstart_calls)
    e2, log2, _, _ = auto_run(loop_on=False, blocks=200)
    check(e2.vstarts == 0, "no autostart without LOOP")
    print("PASS" if not fails else f"{fails} FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
