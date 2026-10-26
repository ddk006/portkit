# Hooks for the bluebox simulation (IronPython 2.7, loaded by sim/bluebox_boot.resc). The image is never changed.
import clr
from Antmicro.Renode.Peripherals.CPU import RegisterValue

CM4_CPUID = 0x410FC241


def cpuid_r3(cpu):
    """After the M4's CPUID load (ldr r3, [r3]): the CM4 value instead of Renode's M7 one."""
    cpu.SetRegisterUnsafe(3, RegisterValue.Create(CM4_CPUID, 32))


def m4_wfe(cpu, machine):
    """WFE in HAL_PWREx_EnterSTOPMode (D2): halt until the M7 releases HSEM 0 (sim/PortkitHSEM.cs resumes)."""
    h = machine['sysbus.hsem2']
    cpu.PC = RegisterValue.Create(int(cpu.PC.RawValue) + 2, 32)
    if not h.Released0:
        h.M4Waiting = True
        cpu.IsHalted = True


_seen = {}


def once(cpu, label, regs=(0, 1)):
    """Print the given registers the first time this hook fires (cheap enough for per-block hooks)."""
    if label in _seen:
        return
    _seen[label] = True
    print("WATCH %s %s" % (label, " ".join("r%d=0x%x" % (r, int(cpu.GetRegisterUnsafe(r).RawValue)) for r in regs)))


def _cstr(bus, a, n=40):
    if a == 0:
        return ""
    b = bus.ReadBytes(a, n)
    s = ""
    for c in b:
        if c == 0:
            break
        s += chr(c)
    return s


def param_def(cpu):
    """FUN_08135E08(table, id, type, label, min, max, key): one parameter definition (M4)."""
    sp = int(cpu.SP.RawValue)
    bus = cpu.Bus
    lbl = _cstr(bus, int(cpu.GetRegisterUnsafe(3).RawValue))
    mn = bus.ReadDoubleWord(sp) - (1 << 32 if bus.ReadDoubleWord(sp) & 0x80000000 else 0)
    mx = bus.ReadDoubleWord(sp + 4) - (1 << 32 if bus.ReadDoubleWord(sp + 4) & 0x80000000 else 0)
    key = _cstr(bus, bus.ReadDoubleWord(sp + 8))
    print("PARAM 0x%03x type %d %-22s %6d %6d %s" % (int(cpu.GetRegisterUnsafe(1).RawValue),
                                                   int(cpu.GetRegisterUnsafe(2).RawValue), repr(lbl), mn, mx, key))


def param_list(cpu):
    """FUN_08135DB4(table, id, label, names, count, key): a list parameter (M4)."""
    sp = int(cpu.SP.RawValue)
    bus = cpu.Bus
    names, cnt = int(cpu.GetRegisterUnsafe(3).RawValue), bus.ReadDoubleWord(sp)
    first = [_cstr(bus, bus.ReadDoubleWord(names + 4 * k)) for k in range(min(cnt, 4))] if names else []
    print("PARAM 0x%03x list   %-22s %6d        %s %s" % (int(cpu.GetRegisterUnsafe(1).RawValue),
          repr(_cstr(bus, int(cpu.GetRegisterUnsafe(2).RawValue))), cnt, _cstr(bus, bus.ReadDoubleWord(sp + 4)), first))


def m7_sdram_alloc(cpu):
    """M7 malloc, SDRAM path (0x08041418): size in r3, caller = stacked lr at sp + 0xc."""
    sp = int(cpu.SP.RawValue)
    print("ALLOC size 0x%x caller 0x%x at 0x%x" % (int(cpu.GetRegisterUnsafe(3).RawValue),
          cpu.Bus.ReadDoubleWord(sp + 0xc), cpu.Bus.ReadDoubleWord(0x24000004)))


# Key matrix (M4, 0x08138A84 / 0x08138B2C): rows PI8 PI12 PI14 PJ1 driven low one at a time (index at obj + 0x608),
# columns PC13 PD4 PG3 PH7 read through HAL_GPIO_ReadPin 0x08110E00 (its only caller is the key poll), low = pressed.
# The hook answers ReadPin itself: 0 if the key at (active row, this column) is down, else 1.
COLS = [(0x58020800, 0x2000), (0x58020C00, 0x10), (0x58021800, 0x8), (0x58021C00, 0x80)]
down = set()


def key_read(cpu):
    port, pin = int(cpu.GetRegisterUnsafe(0).RawValue), int(cpu.GetRegisterUnsafe(1).RawValue) & 0xFFFF
    obj = int(cpu.GetRegisterUnsafe(7).RawValue)                # caller 0x08138A84 keeps the key object in r7
    row = cpu.Bus.ReadDoubleWord(obj + 0x608) & 3
    col = COLS.index((port, pin)) if (port, pin) in COLS else -1
    cpu.SetRegisterUnsafe(0, RegisterValue.Create(0 if (row, col) in down else 1, 32))
    cpu.PC = RegisterValue.Create(int(cpu.GetRegisterUnsafe(14).RawValue) & ~1, 32)


def key(row, col, pressed):
    if pressed:
        down.add((row, col))
    else:
        down.discard((row, col))


def stream_read(cpu):
    """Stream cache read 0x0805B350(this, ?, int64 pos r2:r3, [sp] asset, ..., [sp+0xc] count): first calls only."""
    n = _seen.get("sr", 0)
    if n >= 6:
        return
    _seen["sr"] = n + 1
    sp = int(cpu.SP.RawValue)
    r = lambda k: int(cpu.GetRegisterUnsafe(k).RawValue)
    print("WATCH read this=0x%x r1=0x%x pos=0x%x asset=%d a1=0x%x a2=0x%x cnt=%d lr=0x%x" % (
        r(0), r(1), (r(3) << 32) | r(2), cpu.Bus.ReadDoubleWord(sp), cpu.Bus.ReadDoubleWord(sp + 4),
        cpu.Bus.ReadDoubleWord(sp + 8), cpu.Bus.ReadDoubleWord(sp + 0xc), r(14)))


def stream_trace(cpu):
    """Stream cache read 0x0805B350: report jumps back in the read position (loop wraps) and a sparse trace."""
    r = lambda k: int(cpu.GetRegisterUnsafe(k).RawValue)
    pos = (r(3) << 32) | r(2)
    asset = cpu.Bus.ReadDoubleWord(int(cpu.SP.RawValue))
    key = "tr%d" % asset
    last, n = _seen.get(key, (None, 0))
    if last is not None and pos < last:
        print("WATCH jump asset %d: %d -> %d" % (asset, last, pos))
    elif n % 1500 == 0:
        print("WATCH pos asset %d: %d" % (asset, pos))
    _seen[key] = (pos, n + 1)
