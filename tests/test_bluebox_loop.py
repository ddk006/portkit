"""Unicorn test of the bluebox looper (bluebox/src/loop_m7.c, loop_m4.c, loop_ui_m4.c) on the built image.

  python3 bluebox/build.py loop && .venv/bin/python -P tests/test_bluebox_loop.py

The M7 engine (loop_process, the channel node's process after the EQ) runs block by block against models of the
stock pieces it calls (re/bluebox.md):
  - queue / events: a list of (track, id, value) events for the block;
  - bus: 32 frames, stereo buffers filled with the track's input (a sine, or silence);
  - transport: float tempo +0x18, int64 ticks +0x30, advancing with the blocks;
  - stream cache: write(asset, l, r, n) appends to the asset's file, the asset map entry says whether the file is
    open, length(asset) is -1 until the file is closed and opened for reading (the M4 requests are carried out by the
    test, a few blocks later), read(pos, asset, l, r, n) returns the file's samples or fails past its end.
Checked:
  - the LOOP key arms a slot and asks for the file; recording starts on the next bar line, not before, and records
    the loop + XF + 64 samples of the track's input, then asks to close / reopen the file;
  - playback joins at the transport's phase and stays on it, jumps every bar (no drift over many passes at 133 BPM),
    and the output stays smooth across the jump (crossfade into the head, the file holds the continuation);
  - the LOOP key while playing fades over FADE bars (smooth, to zero), then the slot is free; a new recording of the
    same track during the fade takes the other slot and holds the input only, never the loop; FADE 0 ends in a short
    ramp; the key while recording cancels;
  - two slots; a key press while both are busy changes nothing; FILTER < 0 low-passes the loop;
  - M4: the file requests go to the stock stream manager (open for writing with 2 channels, close + open for reading,
    close on cancel) and are cleared; parameters are defined after the stock ones.
"""
import math
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, \
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_CPU_ARM_MAX

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "out", "bluebox", "loop", "BLUEEURO.BIN")
BASE, SRAM4, STATE = 0x08040000, 0x38000000, 0x3800F000
STACK, RAM, RET = 0x20010000, 0x24000000, 0x24000000
NODE, CTX, TRANSPORT, BUS, BUFL, BUFR = 0x24001000, 0x24001100, 0x24001200, 0x24001300, 0x24002000, 0x24002400
CACHE = 0x10044270
FW = dict(stock=0x080574EC, queue=0x08053C8C, next_ev=0x0804E938, bus=0x08053CD4, frames=0x0804D4C0,
          stereo=0x0804D598, cwrite=0x0805AD08, cread=0x0805B350, clen=0x0805AC88, creq=0x0805B210)
TABLE = 0x10048000                                  # the file slot's block table (all entries 0xFFFF: not listed)
DATA = 0x60000000                                   # block data (floats), 0x40000 bytes per block and channel
BLOCK = 0x9800
M4FW = dict(def_=0x08135E08, add=0x081205E2, open_w=0x0810A840, close=0x0810A8FC, open_r=0x0810A630,
            redraw=0x0813AA9E)
N = 32
XF = 256
LS_IDLE, LS_ARMED, LS_REC, LS_CLOSING, LS_PLAY, LS_FADE = range(6)
REQ_NONE, REQ_OPEN_W, REQ_CLOSE, REQ_CANCEL = range(4)
# struct loop_shared (src/loop_shared.h): magic, cmd[12], chbars[12], chfilter[12] (int16), chfade[12], blocks, slots
OFF_CMD, OFF_SLOT = 4, 72
SLOT_FMT = "<BBBBHBxiiiIi4xQQiif4fiiiI4x"
SLOT_SIZE = struct.calcsize(SLOT_FMT)
SLOT_KEYS = ("state", "track", "gen", "req", "asset", "bars", "written", "target", "start", "clock", "armed_bar", "lenq",
             "posq", "jump", "xf_left", "gain", "ic0", "ic1", "ic2", "ic3", "mode0", "fade_left", "fade_total",
             "reads_failed")


def symbol(name, elf):
    import subprocess
    out = subprocess.run(["arm-none-eabi-nm", os.path.join(ROOT, "out", "bluebox", elf)],
                         capture_output=True, text=True, check=True).stdout
    return next(int(l.split()[0], 16) for l in out.splitlines() if l.split()[-1] == name)


def goertzel(x, f):
    w = 2 * math.pi * f / 48000
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0)) * 2 / len(x)


def sine(f, a=0.5):
    w = 2 * math.pi * f / 48000
    return lambda i: a * math.sin(w * i)


class Emu:
    def __init__(self, bpm=120.0, open_delay=3, close_delay=5):
        data = open(IMG, "rb").read()
        # as tests/bb_emu.py: an A-profile CPU in Thumb state with the FPU on runs the Thumb-2 / FPv5 code
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.ctl_set_cpu_model(UC_CPU_ARM_MAX)
        uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        uc.mem_map(BASE, (len(data) + 0xFFFF) & ~0xFFFF)
        uc.mem_write(BASE, data)
        uc.mem_map(SRAM4, 0x10000)
        uc.mem_map(0x20000000, 0x20000)
        uc.mem_map(RAM, 0x10000)
        uc.mem_map(0x10040000, 0x10000)                 # the cache object (asset map) as the M7 sees it
        uc.mem_map(0x30000000, 0x50000)                 # M4 SRAM (stream manager, store)
        uc.mem_map(DATA, 0x1000000)
        uc.mem_map(0xE000E000, 0x1000)                 # SCB / MPU registers (sram4_uncached)
        self.next_desc = 1
        self.next_data = DATA
        uc.mem_write(RET, b"\x00\xbe")                  # bkpt: end of a call
        uc.mem_write(CACHE + 0x2708, b"\xff" * 0x400)   # no asset has a file
        uc.mem_write(TABLE, b"\xff" * 0x1000)
        uc.mem_write(CACHE + 0x352C + 4 * 7, struct.pack("<I", TABLE))   # file slot 7 (the map entry below)
        self.requests = []
        for a in list(FW.values()) + list(M4FW.values()):
            uc.hook_add(UC_HOOK_CODE, self.stub, begin=a, end=a)
        self.sym = symbol("loop_process", "loop7.elf")
        self.bpm, self.ticks = bpm, 0.0
        self.files = {}                                 # asset -> ([left], [right])
        self.readable = set()
        self.pending = []                               # (block due, action, asset)
        self.open_delay, self.close_delay = open_delay, close_delay
        self.events, self.block_no = [], 0
        self.inp = lambda i: 0.0                        # input sample at absolute sample index i
        self.sample = 0
        self.out_l = []
        self.m4_calls = []
        uc.mem_write(CTX, struct.pack("<I", TRANSPORT))
        uc.mem_write(NODE + 0x1E, struct.pack("<H", 0))

    # --- models of the stock functions
    def stub(self, uc, addr, size, _):
        r = lambda k: uc.reg_read(k)
        sp = r(UC_ARM_REG_SP)
        ret = 0
        if addr == FW["queue"]:
            ret = 0x24004000
        elif addr == FW["next_ev"]:
            i, dst = r(UC_ARM_REG_R1), r(UC_ARM_REG_R2)
            if i < len(self.events):
                track, pid, val = self.events[i]
                uc.mem_write(dst, struct.pack("<B7xIHxxiI", 0x39, track, pid, val, 0))
                ret = 1
        elif addr == FW["bus"]:
            ret = BUS
        elif addr == FW["frames"]:
            ret = N
        elif addr == FW["stereo"]:
            uc.mem_write(r(UC_ARM_REG_R1), struct.pack("<I", BUFL))
            uc.mem_write(r(UC_ARM_REG_R2), struct.pack("<I", BUFR))
        elif addr == FW["cwrite"]:
            asset, l, rr = r(UC_ARM_REG_R1), r(UC_ARM_REG_R2), r(UC_ARM_REG_R3)
            n = struct.unpack("<i", bytes(uc.mem_read(sp, 4)))[0]
            f = self.files.setdefault(asset, ([], []))
            f[0].extend(struct.unpack(f"<{n}f", bytes(uc.mem_read(l, 4 * n))))
            f[1].extend(struct.unpack(f"<{n}f", bytes(uc.mem_read(rr, 4 * n))))
            ret = 1
        elif addr == FW["cread"]:
            pos = (r(UC_ARM_REG_R3) << 32) | r(UC_ARM_REG_R2)
            asset, l, rr, n = struct.unpack("<IIIi", bytes(uc.mem_read(sp, 16)))
            f = self.files.get(asset)
            if asset in self.readable and f and 0 <= pos and pos + n <= len(f[0]):
                uc.mem_write(l, struct.pack(f"<{n}f", *f[0][pos:pos + n]))
                uc.mem_write(rr, struct.pack(f"<{n}f", *f[1][pos:pos + n]))
                ret = 1
        elif addr == FW["creq"]:
            self.requests.append((r(UC_ARM_REG_R1), r(UC_ARM_REG_R2)))
            self.pending.append((self.block_no + 3, "load", (r(UC_ARM_REG_R1), r(UC_ARM_REG_R2))))
        elif addr == FW["clen"]:                        # length(int64 *out, cache, asset)
            out, cache, asset = r(UC_ARM_REG_R0), r(UC_ARM_REG_R1), r(UC_ARM_REG_R2)
            assert cache == CACHE, "length() called with the cache as first argument"
            n = len(self.files[asset][0]) if asset in self.readable else -1
            uc.mem_write(out, struct.pack("<q", n))
            ret = out
        elif addr in (M4FW["def_"], M4FW["add"]):
            self.m4_calls.append(("def" if addr == M4FW["def_"] else "add", r(UC_ARM_REG_R1), r(UC_ARM_REG_R2)))
        elif addr in (M4FW["open_w"], M4FW["close"], M4FW["open_r"]):
            kind = {M4FW["open_w"]: "open_w", M4FW["close"]: "close", M4FW["open_r"]: "open_r"}[addr]
            if kind == "close":
                self.m4_calls.append((kind, r(UC_ARM_REG_R1), "", 0, 0))
            else:
                path = bytes(uc.mem_read(r(UC_ARM_REG_R2), 16)).split(b"\0")[0].decode()
                last = uc.mem_read(sp, 1)[0]
                self.m4_calls.append((kind, r(UC_ARM_REG_R1), path, r(UC_ARM_REG_R3), last))
        uc.reg_write(UC_ARM_REG_R0, ret & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def call(self, sym, *args):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, STACK - 64)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(reg, v)
        uc.emu_start(sym | 1, RET, count=2000000)
        return uc.reg_read(UC_ARM_REG_R0)

    # --- shared state
    def slot(self, i):
        v = struct.unpack(SLOT_FMT, bytes(self.uc.mem_read(STATE + OFF_SLOT + i * SLOT_SIZE, SLOT_SIZE)))
        return dict(zip(SLOT_KEYS, v))

    def key(self, track=0):
        self.uc.mem_write(STATE + OFF_CMD + track, b"\x01")

    def m4_service(self):
        """the M4's part, as loop_tick does it: open / close the files a few blocks after the request"""
        for i in range(2):
            sl = self.slot(i)
            if sl["req"] == REQ_OPEN_W:
                self.pending.append((self.block_no + self.open_delay, "open_w", sl["asset"]))
            elif sl["req"] == REQ_CLOSE:
                self.pending.append((self.block_no + self.close_delay, "readable", sl["asset"]))
            if sl["req"] != REQ_NONE:
                self.uc.mem_write(STATE + OFF_SLOT + i * SLOT_SIZE + 3, b"\x00")
        for p in [p for p in self.pending if p[0] <= self.block_no]:
            self.pending.remove(p)
            _, act, asset = p
            if act == "open_w":
                self.files[asset] = ([], [])
                self.readable.discard(asset)
                self.uc.mem_write(CACHE + 0x2708 + 2 * asset, struct.pack("<H", 7))
                self.uc.mem_write(TABLE, b"\xff" * 0x1000)
            elif act == "readable":
                self.readable.add(asset)
                for b in range(2):                      # the M4 loads the head of the file when opening it
                    self.load(asset, b)
            elif act == "load" and asset[0] in self.readable:
                self.load(*asset)

    def load(self, asset, b):
        """the M4 loader: block b of the file into a descriptor pair (state 2), entered in the table"""
        f = self.files[asset]
        if b * BLOCK >= len(f[0]):
            return
        n = min(BLOCK, len(f[0]) - b * BLOCK)
        uc = self.uc
        el, er = self.next_desc, self.next_desc + 1
        self.next_desc += 2
        for e, ch in ((el, 0), (er, 1)):
            addr = self.next_data
            self.next_data += 0x40000
            uc.mem_write(addr, struct.pack(f"<{n}f", *f[ch][b * BLOCK:b * BLOCK + n]))
            uc.mem_write(CACHE + 24 * e + 4, struct.pack("<IIII", addr, b, 0, n))
            uc.mem_write(CACHE + 24 * e + 0x14, struct.pack("<HHBB", asset, er if e == el else 0xFFFF, 1, 0))
        uc.mem_write(TABLE + 2 * (b + 0x400 if b < 32 else b & 0x3FF), struct.pack("<H", el))

    def block(self, track=0):
        uc = self.uc
        spt = 48000 * 60 / (960 * self.bpm)              # samples per tick
        uc.mem_write(TRANSPORT + 0x18, struct.pack("<f", self.bpm))
        uc.mem_write(TRANSPORT + 0x30, struct.pack("<q", int(self.ticks)))
        x = [self.inp(self.sample + j) for j in range(N)]
        uc.mem_write(BUFL, struct.pack(f"<{N}f", *x))
        uc.mem_write(BUFR, struct.pack(f"<{N}f", *x))
        uc.mem_write(NODE + 0x18, struct.pack("<I", track))
        self.m4_service()
        self.call(self.sym, NODE, CTX)
        if track == 0:
            self.out_l.extend(struct.unpack(f"<{N}f", bytes(uc.mem_read(BUFL, 4 * N))))
            self.events = []
            self.sample += N
            self.ticks += N / spt
            self.block_no += 1

    def run(self, blocks, track=0):
        for _ in range(blocks):
            self.block(track)


def main():
    fails = 0

    def check(ok, what):
        nonlocal fails
        print(("ok   " if ok else "FAIL ") + what)
        fails += not ok

    bar = 96000                                          # 1 bar at 120 BPM, samples
    # --- arming, recording on the bar, length, file requests
    e = Emu()
    e.inp = sine(440)
    e.events = [(0, 0x104, 1)]                          # BARS 1
    e.run(10)
    e.key(0)
    e.block()
    s0 = e.slot(0)
    check(s0["state"] == LS_ARMED and s0["track"] == 0 and s0["asset"] == 0x1FD,
          "LOOP key: slot 1 armed for track 1, asset 0x1FD: state %d asset %#x" % (s0["state"], s0["asset"]))
    e.run(5)
    check(e.slot(0)["state"] == LS_ARMED and not e.files.get(0x1FD, ([], []))[0],
          "armed, file open: nothing recorded before the bar line")
    e.run(-(-bar // N) - e.block_no + 1)                # up to and including the first block of bar 1
    s0 = e.slot(0)
    file_at_bar = len(e.files[0x1FD][0]) - N
    check(s0["state"] == LS_REC and s0["start"] == file_at_bar == 0,
          "recording starts with the first block of the next bar, at the start of the file")
    rec_start = e.sample - N
    e.run(bar // N + 20)
    s0 = e.slot(0)
    target = s0["start"] + bar + XF + 64
    f = e.files[0x1FD][0]
    rec = f[s0["start"]:]
    check(s0["target"] == target and len(f) >= target and s0["state"] >= LS_CLOSING,
          "records the loop + XF + 64 past its start (file %d of %d samples), then closes: state %d"
          % (len(f), s0["target"], s0["state"]))
    check(max(abs(a - sine(440)(rec_start + j)) for j, a in enumerate(rec)) < 1e-6,
          "the file holds the track's input from the bar line")
    check(s0["lenq"] == bar << 16, "loop length 1 bar = %d samples (48.16: %#x)" % (bar, s0["lenq"]))

    # --- playback: phase, jumps, smoothness (input silent from now on)
    e.inp = lambda i: 0.0
    e.out_l = []
    out_start = e.sample
    e.run(int(4.2 * bar / N))
    s0 = e.slot(0)
    check(s0["state"] == LS_PLAY, "playing once the file is readable")
    o = e.out_l
    sound = [k for k in range(len(o)) if abs(o[k]) > 1e-4]
    first = sound[0] if sound else 0
    errs = [abs(o[k] - rec[(out_start + k - rec_start) % bar]) for k in range(first + 64, len(o))
            if (out_start + k - rec_start) % bar >= XF]
    check(sound and max(errs) < 1e-4, "joins at the transport's phase and stays on it (max error %.1e over %.1f "
          "bars; sound %d samples after the recording ended)" % (max(errs) if errs else 9, len(o) / bar,
                                                                 out_start + first - rec_start - target))
    d2 = max(abs(o[k + 1] - 2 * o[k] + o[k - 1]) for k in range(first + 2, len(o) - 1))
    sine_d2 = 0.5 * (2 * math.pi * 440 / 48000) ** 2
    check(d2 < 3 * sine_d2, "smooth across the jumps (second difference %.2f x that of the sine)" % (d2 / sine_d2))
    check(s0["reads_failed"] == 0, "no block missing while playing (%d)" % s0["reads_failed"])

    # --- 133 BPM, 3 bars: a bar is not a whole number of samples; no drift
    e2 = Emu(bpm=133.0)
    e2.inp = sine(440)
    e2.events = [(0, 0x104, 3)]
    e2.run(4)
    e2.key(0)
    exact = 3 * 4 * 60 * 48000 / 133
    e2.run(int((exact / 3 + exact) / N) + 400)
    e2.inp = lambda i: 0.0
    check(abs(e2.slot(0)["lenq"] / 65536 - exact) < 1e-3, "133 BPM 3 bars: %.3f samples (exact %.3f)"
          % (e2.slot(0)["lenq"] / 65536, exact))
    jumps, prev = [], None
    for _ in range(int(7 * exact / N)):
        e2.block()
        p = e2.slot(0)["posq"]
        if prev is not None and p < prev:
            jumps.append(e2.sample)
        prev = p
    gaps = [b - a for a, b in zip(jumps, jumps[1:])]
    check(len(gaps) >= 4 and abs((jumps[-1] - jumps[0]) / len(gaps) - exact) < N / len(gaps) + 1,
          "jumps every %.2f samples over %d passes (exact %.2f, no drift)"
          % ((jumps[-1] - jumps[0]) / max(1, len(gaps)), len(gaps), exact))

    # --- fade, a new recording during the fade, slots
    e.events = [(0, 0x107, 1)]                          # FADE 1 bar
    e.block()
    e.key(0)
    e.block()
    check(e.slot(0)["state"] == LS_FADE, "LOOP key while playing: fade")
    e.out_l = []
    e.inp = sine(1000, 0.3)
    e.key(0)
    e.block()
    s1 = e.slot(1)
    check(s1["state"] == LS_ARMED and s1["asset"] == 0x1FF, "a new recording takes slot 2 (asset %#x) during the fade"
          % s1["asset"])
    e.run(int(1.1 * bar / N))
    check(e.slot(0)["state"] == LS_IDLE, "the fade ends after FADE bars and frees slot 1")
    fade = [v - sine(1000, 0.3)(i) for i, v in zip(range(e.sample - len(e.out_l), e.sample), e.out_l)]
    lv = [max(abs(v) for v in fade[k:k + 4800]) for k in range(0, bar, 4800)]
    check(lv[0] > 0.3 and all(b <= a + 0.03 for a, b in zip(lv, lv[1:])) and lv[-1] < 0.02,
          "the fade falls smoothly to zero: %s" % ["%.2f" % v for v in lv[::4]])
    e.run(int(1.2 * bar / N))
    new = e.files.get(0x1FF, ([], []))[0]
    g440, g1k = (goertzel(new[:24000], 440), goertzel(new[:24000], 1000)) if len(new) > 24000 else (1, 0)
    check(g1k > 0.25 and g440 < 0.002, "the new recording holds the input only (1 kHz %.3f, 440 Hz loop %.4f)"
          % (g1k, g440))
    e.key(1)
    e.block(track=1)
    e.key(2)
    e.block(track=2)
    states = [(e.slot(i)["state"], e.slot(i)["track"]) for i in range(2)]
    check(LS_ARMED in [st for st, tr in states if tr == 1] and 2 not in [tr for st, tr in states if st != LS_IDLE],
          "two slots: track 2 gets the free one, track 3 none: %s" % states)

    e3 = Emu()
    e3.inp = sine(440)
    e3.events = [(0, 0x104, 2)]
    e3.run(2)
    e3.key(0)
    e3.run(bar // N + 50)
    check(e3.slot(0)["state"] == LS_REC, "recording")
    e3.key(0)
    e3.block()
    s = e3.slot(0)
    check(s["state"] == LS_IDLE and s["req"] in (REQ_CANCEL, REQ_NONE), "LOOP key while recording cancels")

    # --- FILTER low-pass, FADE 0
    e4 = Emu()
    e4.inp = lambda i: 0.4 * math.sin(2 * math.pi * 200 * i / 48000) + 0.4 * math.sin(2 * math.pi * 6000 * i / 48000)
    e4.events = [(0, 0x104, 1), (0, 0x107, 0), (0, 0x105, -700)]
    e4.run(2)
    e4.key(0)
    e4.run(int(2.2 * bar / N))
    e4.inp = lambda i: 0.0
    e4.out_l = []
    e4.run(int(0.6 * bar / N))
    o4 = e4.out_l[4000:28000]
    g200, g6k = goertzel(o4, 200), goertzel(o4, 6000)
    check(g200 > 0.3 and g6k < 0.02, "FILTER -700 low-passes the loop: 200 Hz %.3f, 6 kHz %.4f" % (g200, g6k))
    e4.key(0)
    e4.out_l = []
    e4.run(20)
    check(e4.slot(0)["state"] == LS_IDLE and max(abs(v) for v in e4.out_l[-64:]) < 1e-6,
          "FADE 0: the loop ends after a short ramp")

    # --- a block given back by the cache (descriptor data -1) is not read: silence and a request instead
    e5 = Emu()
    e5.inp = sine(440)
    e5.events = [(0, 0x104, 1)]
    e5.run(2)
    e5.key(0)
    e5.run(int(2.05 * bar / N))
    e5.inp = lambda i: 0.0
    s5 = e5.slot(0)
    check(s5["state"] == LS_PLAY, "playing")
    b_first = s5["start"] // BLOCK                      # the loop's first block, left without data
    desc = 0x100
    e5.uc.mem_write(TABLE + 2 * (b_first + 0x400), struct.pack("<H", desc))
    e5.uc.mem_write(CACHE + 24 * desc + 4, struct.pack("<IIII", 0xFFFFFFFF, b_first, 0, 0x9800))
    e5.uc.mem_write(CACHE + 24 * desc + 0x14, struct.pack("<HHBB", s5["asset"], 0xFFFF, 1, 0))
    e5.requests = []
    failed0 = s5["reads_failed"]
    e5.run(int(1.1 * bar / N))                          # passes the loop start
    s5 = e5.slot(0)
    check(s5["reads_failed"] > failed0 and (s5["asset"], b_first) in e5.requests and s5["state"] == LS_PLAY,
          "a block without data is not read (%d blocks silent) but requested; the loop plays on"
          % (s5["reads_failed"] - failed0))
    pre = [b for a, b in e.requests if a == 0x1FD]
    st_b = e.slot(0)["start"] // BLOCK
    check(st_b in pre and st_b + 1 in pre, "the loop start's blocks are requested ahead of the jump (first run: %d requests)"
          % len(pre))

    # --- SRAM4 uncached on the M7 (MPU region 13), set once
    rasr = struct.unpack("<IIII", bytes(e.uc.mem_read(0xE000ED94, 16)))
    check(rasr[1] == 13 and rasr[2] == 0x38000000 and rasr[3] == ((1 << 28) | (3 << 24) | (1 << 19) | (15 << 1) | 1),
          "M7: MPU region 13 makes SRAM4 non-cacheable (RNR %d, RBAR %#x, RASR %#x)" % rasr[1:])


    m4 = Emu()
    m4.uc.mem_write(STATE, struct.pack("<I", 0x384F4F4C))
    base = lambda i: STATE + OFF_SLOT + i * SLOT_SIZE
    m4.uc.mem_write(base(0) + 2, bytes([1, REQ_OPEN_W]) + struct.pack("<H", 0x1FD))
    m4.uc.mem_write(base(1) + 2, bytes([0, REQ_CLOSE]) + struct.pack("<H", 0x1FE))
    tick = symbol("loop_tick", "loop4.elf")
    m4.call(tick, 0x30010000, 0)
    calls = [c for c in m4.m4_calls if c[0] in ("open_w", "close", "open_r")]
    check(calls == [("open_w", 0x1FD, "LOOPA1.WAV", 2, 1), ("close", 0x1FE, "", 0, 0),
                    ("open_r", 0x1FE, "LOOPB0.WAV", 1, 1)],
          "M4: open for writing (2 channels), close + open for reading: %s" % calls)
    check(m4.slot(0)["req"] == REQ_NONE and m4.slot(1)["req"] == REQ_NONE, "M4: requests cleared")
    m4.m4_calls = []
    m4.uc.mem_write(base(0) + 3, bytes([REQ_CANCEL]))
    m4.call(tick, 0x30010000, 0)
    check([c for c in m4.m4_calls if c[0] in ("open_w", "close", "open_r")] == [("close", 0x1FD, "", 0, 0)],
          "M4: cancel closes the file only")
    # --- M4: garbage in SRAM4 at boot (power-on) must not be followed; the Track page's ctor resets the state
    g = Emu()
    g.uc.mem_write(0x3800A000, b"\xa5" * 0x6000)        # UI struct and shared state: garbage, as after power-on
    g.call(tick, 0x30010000, 0)                         # the UI loop runs before the Track page exists
    check(True, "M4: loop_tick with garbage in SRAM4 returns (no widget access)")
    g.call(symbol("loop_ui_reset", "loop4.elf"))
    rd = lambda a: struct.unpack("<I", bytes(g.uc.mem_read(a, 4)))[0]
    check(rd(0x3800A000) == 0 and rd(STATE) == 0, "M4: loop_ui_reset invalidates the UI and the shared state")
    g.uc.mem_write(0x3800A000, b"\xa5" * 0x6000)
    seen = []
    def at_stock_ctor(uc, addr, size, _):            # the stock Track page ctor: stop there, the reset must be done
        seen.append((rd(0x3800A000), rd(STATE)))
        uc.emu_stop()
    g.uc.hook_add(UC_HOOK_CODE, at_stock_ctor, begin=0x08133470, end=0x08133470)
    g.uc.reg_write(UC_ARM_REG_SP, 0x3000F000)
    g.uc.reg_write(UC_ARM_REG_LR, 0x3000F001)
    g.uc.emu_start(symbol("loop_track_ctor", "loop4.elf") | 1, 0x3000F000, count=10000)
    check(seen == [(0, 0)], "M4: loop_track_ctor resets UI and shared state before the stock ctor: %s" % seen)
    m4.uc.mem_write(0x3800A000, struct.pack("<I", 0x4955504C))
    m4.call(symbol("loop_defs", "loop4.elf"), 0x30000000, 0x117, 4, 0x08149000)
    check(struct.unpack("<I", bytes(m4.uc.mem_read(0x3800A000, 4)))[0] == 0x4955504C,
          "M4: loop_defs (runs after the Track page) leaves the Loop view valid")
    m4.m4_calls = []
    for name, args in (("loop_defs", (0x30000000, 0x117, 4, 0x08149000)), ("loop_set_add", (0x30000100, 0x2A, 500))):
        m4.call(symbol(name, "loop4.elf"), *args)
    defs = [(k, hex(i)) for k, i, _ in m4.m4_calls]
    check(defs == [("def", "0x117"), ("def", "0x103"), ("def", "0x104"), ("def", "0x105"), ("def", "0x107"),
                   ("add", "0x2a"), ("add", "0x103"), ("add", "0x104"), ("add", "0x105"), ("add", "0x107")],
          "M4: stock call first, then LOOP, BARS, FILTER, FADE: %s" % defs)
    print("PASS" if not fails else f"{fails} FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
