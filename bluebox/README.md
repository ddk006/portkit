# bluebox eurorack 1.5.1: patches

Stock image `firmware/BLUEEURO.BIN` (SHA-256 in `build.py`; never committed). Findings: `re/bluebox.md`.

```
python3 bluebox/build.py --null                       # no features: must be byte-identical to stock
python3 bluebox/build.py loop                         # -> out/bluebox/loop/BLUEEURO.BIN, build.json
.venv/bin/python -P tests/test_bluebox_loop.py        # Unicorn test of the looper (M7 engine, M4 files)
```

The build checks every patch site against its stock bytes, that every changed byte lies in a registered range and
that every BL lands on its symbol. The version string on the settings page marks the build (`1.5.1L9`).

| Feature | Where | What | Status |
|---|---|---|---|
| `loop` engine | channel node after the EQ | records a track's output into its own WAV on the SD card from the next bar line for BARS bars, then loops it into the same track (crossfaded, FILTER, FADE); independent of the track recorder; 2 slots; M7 cave 0x0808C000, vtable 0x0806BF80 -> `loop_process` | Unicorn: 34/34; Renode: see below; not on hardware |
| `loop` files | M4 UI loop | opens / closes the loop files at the stock stream manager; M4 cave 0x0808E000, BL 0x081351A6 -> `loop_tick` | Unicorn, Renode |
| `loop` UI | Track page, B key | Loop view: FILTER / BARS / FADE knobs with their values, LOOP button showing the state; BL 0x08135BA2, 0x08135718, 0x08135720, page vtable 0x0814E608 | Renode (screenshots) |
| `loop` params | channel parameters | BARS (0x104, 1..16, `chloopbars`), FILTER (0x105, `chloopfilt`), FADE (0x107, 0..16 bars, `chloopfade`), saved with the project; LOOP (0x103, `chloop`) stays defined but unused; BL 0x0813714A -> `loop_defs`, 0x0812087C -> `loop_set_add` | Unicorn |

Boot test builds (L9 still shows a black screen at boot on the device, 2026-11-01): `build.py ver` (version string
only, `1.5.1T1`), `build.py loop_m4` (M4 half: parameters, Loop view, files; `1.5.1T2`), `build.py loop_m7` (M7 half:
the engine on the channel node; `1.5.1T3`). Device: T3 boots, T2 stays black. Split of T2: `build.py
loop_m4_tick` (only the UI loop hook `loop_tick`; `1.5.1T4`), `build.py loop_m4_ui` (T2 without it; `1.5.1T5`). Each goes to `out/bluebox/<name>/BLUEEURO.BIN`.

## On the device

Track page, **B** (Info) twice: block 1 -> block 2 -> **Loop view** -> block 1. Encoders 1 FILTER, 2 BARS, 3 FADE;
the knob labels show the values (`FILTER` / `LP 35%` / `HP 20%`, `BARS 4`, `FADE 2` / `FADE OFF`). BARS and FADE step
once per 96 encoder counts, FILTER 1 % per 8 counts (no acceleration).

**LOOP** button (lit while busy), with the transport running:
1. `ARMED`: the slot's file is opened; recording starts on the next bar line.
2. `REC 1/4`: the track's output (input + clips, after its EQ, before fader / pan) is written for BARS bars + 320
   samples (crossfade).
3. `WAIT`: the file is closed and opened for reading; the loop joins at the bar phase reached meanwhile.
4. `PLAY 2/4`: the loop plays into the track (through its fader, pan and sends, so it is as loud as the track was);
   the live input stays audible under it. FILTER works on the loop.
5. LOOP again: `FADE` over FADE bars while the filter closes towards low-pass (FADE 0: a 5 ms ramp), then free.
   LOOP while `ARMED` / `REC` / `WAIT` cancels.
LOOP during a fade starts a new recording in the other slot. Two slots; with both busy, LOOP does nothing.

Recording tracks or the master with the stock recorder keeps working while loops record and play (Renode: track 3
recorded with REC while track 1 looped). Looping a track that is armed for recording itself is not supported (the
stock routes an armed track differently; not investigated).

Files: `LOOPA0.WAV` / `LOOPA1.WAV` (slot 1), `LOOPB0.WAV` / `LOOPB1.WAV` (slot 2) in the card's root, 24-bit stereo;
each recording overwrites the older file of its slot. Loops do not survive a project change or power-off.

## How it works

State shared by both cores in SRAM4 0x3800F000 (`src/loop_shared.h`), M7 work buffers at 0x38008000, M4 widgets at
0x3800A000. Details and evidence in `re/bluebox.md` ("Looper L8", "L8 black screen").
- SRAM4 after power-on holds garbage, and the M7 caches it by default (no MPU region covers 0x38000000). L9:
  the M4 code touches its widgets only once the UI struct is valid (magic written last by the build); the Track
  page's constructor first invalidates the UI and the shared loop state; the M7 sets up MPU region 13 for SRAM4 as
  shareable, not cacheable (after cleaning its cache lines), so both cores see the same state.
- M7 (`src/loop_m7.c`, `loop_process` replaces the process of channel node 0x0806BF74): takes BARS / FILTER / FADE
  from the track's events; per block it first writes the bus to the slot's asset (0x1FC + 2 slot + generation) via the
  stream cache's write (vtable[0], 0x0805AD08), then adds the playing loop. The loop is read from the cache by our
  code (block table and descriptors, with checks), so a block that is not there yet is silence plus a load request
  (0x0805B210), never a fault. The cache length is `length(&out, cache, asset)` (0x0805AC88).
- M4 (`src/loop_ui_m4.c`, `loop_tick` on the UI loop): carries out the M7's file requests as the stock recorder does:
  open for writing 0x0810A840(sm, asset, path, 2, 1), close 0x0810A8FC(sm, asset), open for reading
  0x0810A630(sm, asset, path, 1, 1); stream manager object 0x3000B778. The LOOP button's tap only sets the M7's
  command byte.

Renode (fresh card, 120 BPM, BARS 1, 440 Hz at the input of track 1, input silent after the take):
- LOOP at tick 1460: file opened, recording from bar line 3840, closed at 7764, loop from 7790 on; output 0.70
  (the loop) through the end of the capture, no missing block, M7 alive.
- Same with track 3 armed and recording (REC) meanwhile: the take of track 3 is written, the loop of track 1 plays.

## Device test (build 1.5.1L9)

L8 installed but the screen stayed black at boot (Kyrill, 2026-11-01). L9 fixes the probable cause (above); that
cause is not proven on the device, and the MPU change has not run on hardware.


1. Keep the stock `BLUEEURO.BIN` (1.5.1) for going back. Copy `out/bluebox/loop/BLUEEURO.BIN` to the SD root and
   install as for bluelex (power on with A held). Settings page shows `1.5.1L9`. If the screen stays black: install the stock file again and report it.
2. PLAY. Track page, B twice. Turn the knobs: values in the labels, BARS / FADE one step at a time.
3. Play into a track, set BARS 2, press LOOP: `ARMED`, then `REC 1/2`, `REC 2/2`, `WAIT`, `PLAY 1/2`... Listen for
   the loop starting in time, clean jumps (no click), the level against the live signal.
4. FILTER both ways while it plays; LOOP again with FADE 2: fade with the filter closing.
5. While a loop plays: arm another track, REC, record a few bars, STOP. The loop keeps playing; the take is there.
6. Two loops on two tracks.
Not checked in the simulation: real SD timing (how long `WAIT` takes; the loop joins at the right phase but may miss
its first moments), CPU load with two loops while recording, SRAM4 use (worked on L7). If the unit hangs at boot:
power on with A held and install the stock file again.
