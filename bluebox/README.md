# bluebox eurorack 1.5.1: patches

Stock image `firmware/BLUEEURO.BIN` (SHA-256 in `build.py`; never committed). Findings: `re/bluebox.md`.

```
python3 bluebox/build.py --null                       # no features: must be byte-identical to stock
python3 bluebox/build.py loop                         # -> out/bluebox/loop/BLUEEURO.BIN, build.json
.venv/bin/python -P tests/test_bluebox_loop.py        # Unicorn test of the loop wrapper
```

The build checks every patch site against its stock bytes, that every changed byte lies in a registered range and
that every BL lands on its symbol. The version string on the settings page marks the build (`1.5.1L2`).

| Feature | Where | What | Status |
|---|---|---|---|
| `loop` (prototype) | track playback | a clip repeats instead of ending (all 12 tracks, mask in SRAM4 0x3800F004), with a 256-sample equal-power crossfade at the loop point; M7 cave 0x0808C000 (1160 B), BL at 0x08057D1E / 0x08057EF8 / 0x08057F06 -> `loop_voice`, stream cache vtable 0x08076BDC -> `loop_read` | Unicorn: 13/13; Renode: loops without drift, no click (max curvature 2.8x a pure sine, was 185-480x), level as stock; not on hardware |

The crossfade needs audio past the loop end in the file (at least 256 samples); the looper will record past the end.
If the file ends at the loop end, up to one block (32 samples) of silence remains at the jump.

Not flashed. Next for the looper: loop length in bars from the tempo, 2 slots with arming/stop, fade out with filter.
