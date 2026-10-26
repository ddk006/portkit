# bluebox eurorack 1.5.1: patches

Stock image `firmware/BLUEEURO.BIN` (SHA-256 in `build.py`; never committed). Findings: `re/bluebox.md`.

```
python3 bluebox/build.py --null                       # no features: must be byte-identical to stock
python3 bluebox/build.py loop                         # -> out/bluebox/loop/BLUEEURO.BIN, build.json
.venv/bin/python -P tests/test_bluebox_loop.py        # Unicorn test of the loop wrapper
```

The build checks every patch site against its stock bytes, that every changed byte lies in a registered range and
that every BL lands on its symbol. The version string on the settings page marks the build (`1.5.1L7`).

| Feature | Where | What | Status |
|---|---|---|---|
| `loop` | track playback | a clip on a track with LOOP on repeats BARS bars from the bar line (2 slots), 256-sample equal-power crossfade at the loop point, FILTER and FADE; M7 cave 0x0808C000 (8 KB), M4 cave 0x0808E000 (8 KB); BL at 0x08057D1E / 0x08057EF8 / 0x08057F06 -> `loop_voice`, stream cache vtable 0x08076BDC -> `loop_read`, player vtables 0x0806BFE8 -> `loop_player`, 0x0806BF80 -> `loop_process`, M4 BL 0x0813714A -> `loop_defs`, 0x0812087C -> `loop_set_add` | Unicorn: 42/42; Renode: see below; not on hardware |
| `loop` UI | Track page, B key | Loop view with FILTER / BARS / FADE knobs and a LOOP button; M4 BL 0x08135BA2, 0x08135718, 0x08135720, page vtable 0x0814E608 | Renode (screenshots); not on hardware |
| `loop` record | REC key | with Rec Enable and LOOP on a track, the take ends BARS bars after the next bar line and loops at once; Rec Enable of that track goes off with the take; M4 BL 0x08135B24 -> `loop_app_msg`, 0x081351A6 -> `loop_tick` | Renode: WAV written, loop audible right after the take; not on hardware |

The crossfade needs audio past the loop end in the file (at least 256 samples); the looper will record past the end.
If the file ends at the loop end, up to one block (32 samples) of silence remains at the jump.

Per track: channel parameters **LOOP** (0x103, on/off, key `chloop`) and **BARS** (0x104, 1..16, key `chloopbars`,
default 4), defined on the M4 (cave 0x0808E000) and kept in the channel set, so projects save them. The M7 picks them
up from the track's queue (`loop_process`, track player vtable 0x0806BF80). Two loop slots in switch-on order; a third
track with LOOP on waits for a free slot. **FILTER** (0x105, -1000..1000, `chloopfilt`): DJ filter on the loop
(< 0 low-pass 20 kHz .. 60 Hz, > 0 high-pass 20 Hz .. 8 kHz, SVF). **FADE** (0x107, 0..16 bars, `chloopfade`,
default 4): LOOP off fades the loop out over FADE bars (cosine) while the filter closes towards low-pass; then the clip
ends and the slot is free (FADE 0: at once, stock fade). LOOP on during the fade cancels it. The track's live input
stays audible under the loop and while it fades (simulation; the stock firmware lowers it by ~3 dB while the clip
plays and brings it back when the clip ends).
The loop starts on the first bar line after the clip starts on the timeline and is BARS bars of 4/4 at the current
tempo (fewer if the clip is too short); jump lengths come from a fixed-point accumulator, so the loop stays on the bar
grid. Tempo changes while a loop plays are not followed yet.

**On the device (Loop view):** on the Track page, the B (Info) key steps block 1 (VOL PAN GAIN CUE) -> block 2 (FX1
FX2 OUT3&4) -> **Loop view** -> block 1. The Loop view shows FILTER, BARS and FADE knobs in block 2's frame (encoders
1 FILTER, 2 BARS, 3 FADE) and a big LOOP button (lit = on); the stock widgets come back when you leave it or open the
Track page again. M4 code `src/loop_ui_m4.c`, widgets and state in SRAM4 0x3800A000.

**Recording a loop:** select the track and set Rec on it (touch), turn LOOP on in the Loop view and set BARS, press
PLAY, then REC. The recording runs from REC as in the stock firmware; it stops by itself at the next bar line (at least
a 16th after REC) + BARS bars (+ a 16th of audio for the crossfade) while the transport keeps running, and the take
starts looping a 16th later from that bar line. Rec Enable of the track is switched off with the take: the stock
player does not play a track's clips while Rec Enable is on (it plays the input instead), so the loop would be silent.
To record again, set Rec again.

How the firmware is bent for this (details and evidence in `re/bluebox.md`):
- Punch In / Out are not used: setting them while the transport runs made the recorder register a take without
  writing its file (simulation). `loop_tick` switches record off itself (0x081272CC(store, 0)).
- The stock plans a track's clips only at PLAY or while a voice plays; `autostart` (M7) moves the new clip to "now +
  a 16th" at the loop's phase and calls the stock planning (0x08057900) itself, repeated for up to 200 blocks while
  the stream cache does not know the file's length yet.
- Finishing a take while the transport runs makes the Mixer node miss ~8 ticks of reads of the recording track
  (7 stream errors in the simulation; the header would show `ERROR 7` instead of the free space). `loop_tick` counts
  stream errors of the first 480 ticks after the take back; errors outside that window stay visible. Probably a ~5 ms
  gap in the monitored input at the end of the take (not verified).

Renode (fresh card, 120 BPM, BARS 1, 440 Hz at the input, REC at tick ~1690, input silent from 4.4 s):
24-bit stereo WAV of 958200 bytes, take end at tick 7920, loop from bar line 3840, 95968 / 96000 samples per pass,
output carries the loop from the take end on (0.70) with the input silent, header without errors.

**Device test (not flashed yet, nothing heard on hardware):**
1. Keep the stock `BLUEEURO.BIN` (1.5.1) for going back. Copy `out/bluebox/loop/BLUEEURO.BIN` to the SD root and
   install as for bluelex (power on with A held; the bluebox flashes the file). Settings page shows `1.5.1L7`.
2. Track page, B twice -> Loop view. On a track with an existing clip: LOOP on, BARS 1/2/4; listen for clean jumps on
   the bar (no click), FILTER both ways, LOOP off with FADE 1..4 (fade while the filter closes), the live input
   audible under the loop.
3. Recording: Rec on a track, LOOP on, BARS 2, PLAY, REC, play something. The recording should end by itself after the
   next bar line + 2 bars and loop right away; the header should show the free space, not `ERROR`.
4. Things I could not check in the simulation: SRAM4 (0x38000000, state and widgets) without its clock enabled, real
   SD timing (the 200-block retry), CPU load with two loops, a click at the end of the take in the monitored input.
   If the unit hangs at boot: power on with A held and install the stock file again.
