//
// STM32H7 dual-core hardware semaphore (HSEM, 0x58026400) for the bluebox simulation. Renode's
// STM32H7_HardwareSemaphore has no second-core interrupt registers (C2IER.. at 0x110) and does not lock with the
// accessing core's id. Here:
//   R[n]   (0x00 + 4n) write: lock if free (COREID of the accessing CPU, PROCID from the value); write with LOCK = 0
//          and matching COREID/PROCID releases. Read: current state.
//   RLR[n] (0x80 + 4n) read: lock if free (PROCID 0), returns the state.
//   C1IER/ICR/ISR/MISR 0x100..0x10C, C2* 0x110..0x11C: a release sets ISR bit n for both cores; ICR clears.
//   CR (0x140): KEY << 16 | COREID << 8 releases all locks of that core; KEYR (0x144) stored.
// COREID: CM7 = 3, CM4 = 1 (RM0399). The accessing CPU is taken from the bus: the one named `m4Name` is the CM4.
// Wake-up of the M4 from its WFE (EnterSTOPMode, D2): see M4Waiting and sim/bluebox_boot.resc.
// IRQ1 = HSEM1 (CM7 NVIC 125), IRQ2 = HSEM2 (CM4 NVIC 126), level = MISR != 0.
//
// Laden: include @sim/PortkitHSEM.cs (vor der Plattformbeschreibung).
//
using Antmicro.Renode.Core;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals.Bus;
using Antmicro.Renode.Peripherals.CPU;

namespace Antmicro.Renode.Peripherals.Miscellaneous
{
    public class PortkitHSEM : IDoubleWordPeripheral, IKnownSize
    {
        public PortkitHSEM(IMachine machine, string m4Name = "cpu1")
        {
            this.machine = machine;
            this.m4Name = m4Name;
            IRQ1 = new GPIO();
            IRQ2 = new GPIO();
            Reset();
        }

        public GPIO IRQ1 { get; }
        public GPIO IRQ2 { get; }
        public long Size => 0x400;
        public bool Trace { get; set; }
        // set by the boot script's WFE hook: the M4 sleeps (halted) until semaphore 0 is released
        public bool M4Waiting { get; set; }
        public bool Released0 { get; private set; }

        public void Reset()
        {
            for(var i = 0; i < 32; i++) sem[i] = 0;
            for(var i = 0; i < 2; i++) ier[i] = isr[i] = 0;
            keyr = 0;
            Update();
        }

        public uint ReadDoubleWord(long offset)
        {
            if(offset < 0x80) return sem[offset / 4];
            if(offset < 0x100)
            {
                var n = (int)(offset - 0x80) / 4;
                if((sem[n] & Lock) == 0) Take(n, CoreId(), 0);
                return sem[n];
            }
            switch(offset)
            {
                case 0x100: return ier[0];
                case 0x108: return isr[0];
                case 0x10C: return isr[0] & ier[0];
                case 0x110: return ier[1];
                case 0x118: return isr[1];
                case 0x11C: return isr[1] & ier[1];
                case 0x144: return keyr;
            }
            return 0;
        }

        public void WriteDoubleWord(long offset, uint value)
        {
            if(offset < 0x80)
            {
                var n = (int)offset / 4;
                var core = CoreId();
                var proc = value & 0xFF;
                if((value & Lock) != 0)
                {
                    if((sem[n] & Lock) == 0) Take(n, core, proc);
                }
                else if((sem[n] & Lock) != 0 && ((sem[n] >> 8) & 0xF) == core && (sem[n] & 0xFF) == proc)
                {
                    Release(n);
                }
                return;
            }
            switch(offset)
            {
                case 0x100: ier[0] = value; break;
                case 0x104: isr[0] &= ~value; break;
                case 0x110: ier[1] = value; break;
                case 0x114: isr[1] &= ~value; break;
                case 0x140:
                    if((value >> 16) == keyr)
                    {
                        var core = (value >> 8) & 0xF;
                        for(var n = 0; n < 32; n++)
                            if((sem[n] & Lock) != 0 && ((sem[n] >> 8) & 0xF) == core) Release(n);
                    }
                    break;
                case 0x144: keyr = value >> 16; break;
            }
            Update();
        }

        private void Take(int n, uint core, uint proc)
        {
            sem[n] = Lock | (core << 8) | proc;
            if(Trace) this.Log(LogLevel.Warning, "take {0} core {1} proc {2}", n, core, proc);
        }

        private void Release(int n)
        {
            if(Trace) this.Log(LogLevel.Warning, "release {0} (was 0x{1:X})", n, sem[n]);
            sem[n] = 0;
            if(n == 0)
            {
                Released0 = true;
                if(M4Waiting && machine.TryGetByName<ICPU>("sysbus." + m4Name, out var m4))
                {
                    M4Waiting = false;
                    m4.IsHalted = false;
                }
            }
            isr[0] |= 1u << n;
            isr[1] |= 1u << n;
            Update();
        }

        private uint CoreId()
        {
            if(machine.SystemBus.TryGetCurrentCPU(out var cpu) && machine.GetLocalName(cpu) == m4Name) return 1;
            return 3;
        }

        private void Update()
        {
            IRQ1.Set((isr[0] & ier[0]) != 0);
            IRQ2.Set((isr[1] & ier[1]) != 0);
        }

        private const uint Lock = 0x80000000;
        private readonly IMachine machine;
        private readonly string m4Name;
        private readonly uint[] sem = new uint[32];
        private readonly uint[] ier = new uint[2];
        private readonly uint[] isr = new uint[2];
        private uint keyr;
    }
}
