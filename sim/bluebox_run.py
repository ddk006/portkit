"""Run the simulated bluebox eurorack (1.5.1, both cores) through a sequence of actions.

  python3 sim/bluebox_run.py [--image stock|<path>] [--sd <FAT32 image>] [--out <dir>] <actions...>
  actions: wait <s> | pc | press <key> [<s>] | hold <key> | up <key> (KEYS) | tap <x> <y> [<s>] | shot <name> | watch <addr> <label> <Read|Write> | fill <addr> <len> <byte> | read <addr> [<n words>] | dump <addr> <len> <name> | mon <Renode command>
  e.g.     python3 sim/bluebox_run.py wait 3 pc

Output (dumps, renode.log) goes to out/sim/bluebox/ (or --out). Renode: ~/tools/renode (see README).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "sim")
RENODE = os.path.expanduser("~/tools/renode/Renode.app/Contents/MacOS/renode")
# Key matrix 4 x 4 (sim/bbhook.py key_read): (row, column); event codes from the table at 0x3001F7D8
# (row r = bytes r*64.., 16 per column). Names = codes until known on the device.
KEYS = {"E0": (0, 0), "E1": (0, 1), "F9_0": (1, 0), "F9_1": (1, 1), "F9_5": (1, 2), "STOP": (1, 3),
        "F9_4": (2, 0), "F9_2": (2, 1), "F9_6": (2, 2), "PLAY": (2, 3), "REC": (3, 0), "F9_3": (3, 1), "F9_7": (3, 2)}
# STOP (code 0xF6), PLAY (0xF7), REC (0xF4 / 0xF5 release): seen in the simulation (2026-10-26)
TOUCH_FLIP_Y = False                                     # orientation of the GT911 y axis: to be checked
SHOTS = []
ROT = ["--rot", "0"]                                     # the LTDC frame is already landscape (320 x 240)
IMAGES = {"stock": os.path.join(ROOT, "firmware", "BLUEEURO.BIN")}


def body(path):
    return [l for l in open(path).read().splitlines() if l.strip() and not l.startswith("#")]


def build(image, out, actions, sd=None):
    lines = [f"include @{os.path.join(SIM, 'PortkitHSEM.cs')}", f"include @{os.path.join(SIM, 'PortkitGT911.cs')}",
             f"include @{os.path.join(SIM, 'PortkitAudioDMA.cs')}",
             f"$bin=@{image}",
             f"$repl=@{os.path.join(SIM, 'bluebox.repl')}"]
    lines += [l.replace("$simdir", SIM) for l in body(os.path.join(SIM, "bluebox_boot.resc"))]
    if sd:
        lines += [l.replace("$simdir", SIM).replace("$sdimg", sd) for l in body(os.path.join(SIM, "sd_bluebox.resc"))]
    lines.append('cpu1 AddHook 0x08110E00 "import bbhook; bbhook.key_read(self)"')
    lines.append("logLevel 3")
    i = 0
    while i < len(actions):
        a = actions[i]
        if a == "wait":
            lines.append(f'emulation RunFor "{actions[i + 1]}"'); i += 2
        elif a == "pc":
            lines += ['python "print(\'M7 / M4 PC:\')"', "cpu PC", "cpu1 PC"]; i += 1
        elif a == "read":                                  # read <addr> [<n words>]
            n = int(actions[i + 2]) if i + 2 < len(actions) and actions[i + 2].isdigit() else 1
            base = int(actions[i + 1], 16)
            lines += [f"sysbus ReadDoubleWord {base + 4 * k:#x}" for k in range(n)]
            i += 3 if i + 2 < len(actions) and actions[i + 2].isdigit() else 2
        elif a == "dump":
            path = os.path.join(out, actions[i + 3] + ".bin")
            lines.append(f'python "open(r\'{path}\', \'wb\').write(bytes(bytearray(self.Machine.SystemBus.ReadBytes({actions[i + 1]}, {actions[i + 2]}))))"')
            i += 4
        elif a == "shot":                                  # LTDC registers + layers -> <out>/<name>/, PNG after the run
            d = os.path.join(out, actions[i + 1]); os.makedirs(d, exist_ok=True); SHOTS.append(d)
            lines += [l.replace("$shot", d) for l in body(os.path.join(SIM, "screenshot.resc"))]
            i += 2
        elif a == "watch":                                 # watch <addr> <label> <Read|Write>: accessing core + PC
            lines.append(f'sysbus AddWatchpointHook {actions[i + 1]} DoubleWord {actions[i + 3]} "ok, c = self.TryGetCurrentCPU(); '
                         f'print(\'WATCH {actions[i + 2]} %s pc=0x%x lr=0x%x\' % (self.GetMachine().GetLocalName(c) if ok else \'?\', '
                         f'c.PC.RawValue if ok else 0, c.LR.RawValue if ok else 0))"')
            i += 4
        elif a == "fill":                                  # fill <addr> <len> <byte>: memory pattern (before boot: first action)
            lines.append(f'python "import System; self.Machine.SystemBus.WriteBytes(System.Array[System.Byte]([{int(actions[i + 3], 16)}] * {int(actions[i + 2], 16)}), {actions[i + 1]})"')
            i += 4
        elif a == "watchv":                                # watchv <addr> <label>: writes with core, PC, LR and value
            lines.append(f'sysbus AddWatchpointHook {actions[i + 1]} DoubleWord Write "ok, c = self.TryGetCurrentCPU(); '
                         f'print(\'WATCH {actions[i + 2]} %s pc=0x%x lr=0x%x val=0x%x\' % (self.GetMachine().GetLocalName(c) if ok else \'?\', '
                         f'c.PC.RawValue if ok else 0, c.LR.RawValue if ok else 0, value))"')
            i += 3
        elif a in ("press", "hold", "up"):                 # press <key> [<s>] | hold <key> | up <key> (names: KEYS)
            r, c = KEYS[actions[i + 1]]
            dur = 0.15
            i += 2
            if a == "press" and i < len(actions) and actions[i].replace(".", "").isdigit():
                dur = float(actions[i]); i += 1
            k = 'python "import bbhook; bbhook.key(%d, %d, %s)"'
            if a in ("press", "hold"):
                lines.append(k % (r, c, "True"))
            if a == "press":
                lines += [f'emulation RunFor "{dur}"', k % (r, c, "False"), 'emulation RunFor "0.15"']
            if a == "up":
                lines.append(k % (r, c, "False"))
        elif a == "tap":                                   # tap <x> <y> [<s>]: screen coordinates (top left)
            x, y = int(actions[i + 1]), int(actions[i + 2])
            dur = 0.15
            i += 3
            if i < len(actions) and actions[i].replace(".", "").isdigit():
                dur = float(actions[i]); i += 1
            uy = 239 - y if TOUCH_FLIP_Y else y
            lines += [f'python "self.Machine[\'sysbus.i2c1.touch\'].Touch({x}, {uy})"', f'emulation RunFor "{dur}"',
                      'python "self.Machine[\'sysbus.i2c1.touch\'].Release()"', 'emulation RunFor "0.2"']
        elif a == "mon":
            lines.append(actions[i + 1]); i += 2
        else:
            sys.exit(f"unknown action {a}")
    lines.append("quit")
    return "\n".join(lines) + "\n"


def main():
    args = sys.argv[1:]
    image, out, sd = IMAGES["stock"], os.path.join(ROOT, "out", "sim", "bluebox"), None
    while args and args[0].startswith("--"):
        if args[0] == "--image":
            image = IMAGES.get(args[1], os.path.abspath(args[1]))
        elif args[0] == "--sd":
            sd = os.path.abspath(args[1])
        elif args[0] == "--out":
            out = os.path.abspath(args[1])
        args = args[2:]
    os.makedirs(out, exist_ok=True)
    resc = os.path.join(out, "run.resc")
    open(resc, "w").write(build(image, out, args, sd))
    log = open(os.path.join(out, "renode.log"), "w")
    p = subprocess.Popen([RENODE, "--console", "--disable-gui", "--plain", resc], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    for line in p.stdout:
        log.write(line)
        if line.startswith(("0x", "M7", "WATCH", "PARAM", "ALLOC", "[WARNING] hsem", "There was an error", "Error")) or "Exception" in line:
            print(line.rstrip())
    p.wait()
    for d in SHOTS:
        subprocess.run([sys.executable, os.path.join(SIM, "screenshot.py"), d, d + ".png"] + ROT, cwd=ROOT)


if __name__ == "__main__":
    main()
