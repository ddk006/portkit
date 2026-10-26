# bluebox eurorack 1.5.1: patches

Stock image `firmware/BLUEEURO.BIN` (SHA-256 in `build.py`; never committed). Findings: `re/bluebox.md`.

```
python3 bluebox/build.py --null                       # no features: must be byte-identical to stock
python3 bluebox/build.py loop                         # -> out/bluebox/loop/BLUEEURO.BIN, build.json
.venv/bin/python -P tests/test_bluebox_loop.py        # Unicorn test of the loop wrapper
```

The build checks every patch site against its stock bytes, that every changed byte lies in a registered range and
that every BL lands on its symbol. The version string on the settings page marks the build (`1.5.1L1`).

| Feature | Where | What | Status |
|---|---|---|---|
| `loop` (prototype) | track playback | a clip repeats instead of ending (all 12 tracks, mask in SRAM4 0x3800F004); M7 cave 0x0808C000 (444 B), BL at 0x08057D1E / 0x08057EF8 / 0x08057F06 -> `loop_voice` | Unicorn: pass; Renode: loops without drift, click at the jump; not on hardware |

Not flashed. Next for the looper: jump inside the block plus a short crossfade, loop length in bars from the tempo,
2 slots with arming/stop, fade out with filter.
