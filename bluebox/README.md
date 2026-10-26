# bluebox eurorack 1.5.1: patches

Stock image `firmware/BLUEEURO.BIN` (SHA-256 in `build.py`; never committed). Findings: `re/bluebox.md`.

```
python3 bluebox/build.py --null                       # no features: must be byte-identical to stock
python3 bluebox/build.py loop                         # -> out/bluebox/loop/BLUEEURO.BIN, build.json
.venv/bin/python -P tests/test_bluebox_loop.py        # Unicorn test of the loop wrapper
```

The build checks every patch site against its stock bytes, that every changed byte lies in a registered range and
that every BL lands on its symbol. The version string on the settings page marks the build (`1.5.1L5`).

| Feature | Where | What | Status |
|---|---|---|---|
| `loop` (prototype) | track playback | a clip on a track with LOOP on repeats BARS bars from the bar line (2 slots), 256-sample equal-power crossfade at the loop point; M7 cave 0x0808C000 (8 KB), M4 cave 0x0808E000 (8 KB); BL at 0x08057D1E / 0x08057EF8 / 0x08057F06 -> `loop_voice`, stream cache vtable 0x08076BDC -> `loop_read`, player vtable 0x0806BF80 -> `loop_process`, M4 BL 0x0813714A -> `loop_defs`, 0x0812087C -> `loop_set_add` | Unicorn: 30/30; Renode: loops without drift, no click (max curvature 2.8x a pure sine, was 185-480x), level as stock, 1 bar at 120 / 133 BPM starts on the bar line with the right length; not on hardware |

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

Not on the panel yet (no widgets): the parameters exist, but there is no button or knob for them. Next: controls on
the Track page, recording that stops after BARS bars. Not flashed.
