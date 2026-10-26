# 1010music bluebox eurorack 1.5.1 (`BLUEEURO.BIN`)

Eingabe: `firmware/BLUEEURO.BIN`, 1 113 628 Bytes,
SHA-256 `fcb04565e2fd5bc15c0b2d0dc913dc99dc74732611e8cef0d7e8e4bb4af7402b` (identisch mit der Basis von
[bluelex](https://github.com/lexnels/bluelex-bluebox-euro), dessen Adressen also direkt gelten).
Werkzeug: `tools/bbx.py` (Adressen der drei Images, Disassembly, Literal-Referenzen, Funktionsanfang).
Simulation: `sim/bluebox_run.py` (siehe `sim/README.md`).

Status: `vermutet` = aus Disassembly gelesen, `emu` = in der Renode-Simulation beobachtet, `hw` = am Gerät gehört.

## Aufbau

STM32H7 mit zwei Kernen (CubeMX-Dual-Core-Muster, HSEM-Handshake). Die Datei ist lückenloser Flash ab 0x08040000:

| Flash | Datei | Inhalt | Vektoren |
|---|---|---|---|
| 0x08040000 | 0x000000 | M7: Audio-Engine, USB-Audio (OTG_HS), SAI1-4 | SP 0x2404DF38, Reset 0x080403CB |
| 0x0808AC00..0x080A0000 | | Nullbytes (bluelex nutzt 0x08090000..0x080E0000 als Code-Caves) | |
| 0x080E0000 | 0x0A0000 | weiteres M7-Image mit LTDC/SD/USB: vermutlich Updater (offen) | SP 0x24017170, Reset 0x080E03CB |
| 0x08100000 | 0x0C0000 | M4: UI (LTDC 320x240, DMA2D), SD/FatFS, ADC (Potis/CV), MIDI, USB-Host | SP 0x3003AAC0, Reset 0x081003CB |

## Boot und Kopplung der Kerne (emu)

- M4 `main` 0x0810D59C: HSEM-Takt, `HAL_HSEM_ActivateNotification(0)` 0x081119A8, `HAL_PWREx_ClearPendingEvent`
  0x08112E38, `HAL_PWREx_EnterSTOPMode(D2, WFE)` 0x08112D98; weiter, wenn der M7 HSEM 0 freigibt.
  `HAL_GetCurrentCPUID` 0x0810EB50 (CPUID & 0xF0 == 0x70 -> CM7 = 3, sonst 1).
- M7 `HAL_PWREx_ControlVoltageScaling` 0x08061388 (VOS0). M7 `main` endet in der Audio-Hauptschleife 0x0804100C
  (Objekt 0x24002CB0, Positionen +0x48/+0x4C aus den SAI-Callbacks).
- Postfach M4 <-> M7 in D2-SRAM3: M4 schreibt bei 0x30040000, der M7 liest über den Alias **0x10040000**
  (Zeiger 0x24009F58, gelesen von 0x0804BEB0 im Audio-Callback 0x0804C778). Queue: 24-Byte-Nachrichten
  (Typ 0x39 = Parameter-Event: id +0x0C, Wert +0x10), Indizes +0x1800 (lesen) / +0x1804 (schreiben).
  Weitere Bereiche: 0x300478C0 ADC-Werte (24 Halbworte), 0x30047938 Zähler des M4.
- M4-Zeitbasis: TIM2 (`HAL_TIM_PeriodElapsedCallback` 0x0810D744 -> `HAL_IncTick` 0x0810D7F4).
- M4-Tasks (FreeRTOS): defaultTask (UI-Schleife 0x08135180, wartet <= 1 s auf eine Semaphore), pcmStreamer
  (SD-Befehle vom M7, Ring im Objekt 0x30017C10, Dispatcher 0x0811FF80), USBH_Thread.

## Peripherie (emu, soweit die Simulation sie braucht)

| | |
|---|---|
| SDRAM | FMC Bank 1, 0xC0000000, 64 MB (Init 0x08041E7C: 9 Spalten-, 13 Zeilen-Bits, 4 Banks, 32 Bit) |
| Display | LTDC 320 x 240 quer, 2 Ebenen ARGB1555, Puffer 0xC3400100 / 0xC344B500 (SDRAM) |
| I2C1 | 0x44, 0x45 (vermutlich zwei Codecs), 0x5D GT911-Touch |
| Audio-DMA | DMA2: S1 RAM 0x2407E800 -> SAI1 A, S5 SAI1 B -> 0x2407F800, S7 SAI2 B -> 0x2407F000, je 0x200 Worte zirkulär, HT/TC; 1 Hälfte = 32 Frames x 8 Slots. S0 ADC1 -> 0x30001820 (M4) |
| SD | Cube `sd_diskio`, `SD_Driver` 0x0814B134: init 0x08104BB8, status 0x08104D90, read 0x08104C50, write 0x08104CC4, ioctl 0x08104D38; Mount `SDMgrCM4` 0x08104E7C |

## Audio-Graph (emu)

Knoten: vtable mit gemeinsamer Basis, Slot 3 = `process(obj, ctx)`, +0x08 `next`, +0x14 Index, +0x18 Queue-Id,
Bus-Port = Halbwort +0x1E (Methode 0x08042334). `ctx` = 0x24033414. Blöcke zu 32 Frames bei 48 kHz.
Bus 0..11 = Kanäle, 12 = Master, 13 = FX1 (Delay), 14 = FX2 (Reverb) (Bus-Nummern 12-14 aus bluelex).
Builder 0x08051148 legt die Knoten nach Typ-Id an.

Reihenfolge (über `next`): `b374` (Eingangs-Routing?) -> `bea4` (Recorder?) -> je Kanal 0..11
`bfdc` -> `ad18` (EQ) -> `bf74` -> `b3e8` -> `c0ac` -> `bf0c` -> `b988` (Mixer: Pan, Sends) -> Reverb -> Delay
-> `b9f0` -> `ad18` (Master-EQ) -> `bf74` -> `c0ac` -> `bf0c` -> Kompressor -> `b450` -> `b4b8`.

| vtable | process | Rolle | Status |
|---|---|---|---|
| 0x0806ACB0 | 0x08042A90 | Master-Kompressor | vermutet (bluelex) |
| 0x0806ADA0 | 0x08048DE8 | Reverb (FX2) | vermutet (bluelex) |
| 0x0806BA64 | 0x08053B30 | Delay (FX1) | vermutet (bluelex) |
| 0x0806AD18 | 0x08043FD8 | EQ (je Kanal und Master) | vermutet |
| 0x0806B988 | 0x08050564 | Mixer (Pan, FX-Sends, Quelle) | vermutet |
| 0x0806BEA4 | 0x080562F0 | Recorder (Rec Enable, Pre/Post) | vermutet |
| 0x0806B374 | 0x0804D958 | Eingangs-Routing (chansource) | vermutet |
| 0x0806BFDC, 0x0806BF74, 0x0806B3E8, 0x0806C0AC, 0x0806BF0C, 0x0806B9F0, 0x0806B450, 0x0806B4B8, 0x0806B920 | | offen | |

Parameter (149, M4-Tabelle, Definition `FUN_08135E08(table, id, type, label, min, max, key)`, Listen
`FUN_08135DB4`): `out/sim/bluebox/params.txt` (Simulation, nicht im Repo). U. a. 12 Kanäle mit `chansource`,
`pregaindb`, 4-Band-EQ `eq*`, `panpos`, `gaindb`, `chmute`/`chsolo`, Sends; Tempo `globtempo` (0x085, 40..250);
Ducker `duck*` (0x01F..0x02B); Track-Recorder mit Transport (`trackplay`, `transpunchin`, ...).

## Speicher (emu)

- M7-`malloc` (0x080413D0 und 0x08041474): klein -> DTCM, dann AXI-Heap, groß -> SDRAM per Bump-Zeiger 0x24000004
  ab 0xC0000000, Abbruch über 0xC3400000. Nach dem Boot: 0xC327E000, **frei ~1,5 MB**.
  - 0x0805B1A8: 288 x 0x26000 = **42,75 MB** (Tabelle mit 0x18-Byte-Einträgen; vermutlich Streaming-Cache der
    Track-Wiedergabe/Aufnahme) + 64 x 0x840
  - Reverb 1 MB (0xC2AE5000), Delay 6 x 0x119800 (0xC2BE5000..)
- M4-Heap (0x08102F4C aufwärts / 0x08102FB4 abwärts, Zeiger 0x3000008C / 0x30000088): 0xC3400000..0xC4000000,
  nach dem Boot 0xC3B223A0, **frei ~4,9 MB** (Canary-Messung bestätigt: oberhalb ~0xC3B10000 unberührt).
- Der M4 löscht beim Start 0xC0000000..0xC3400000.

## Recorder und Track-Wiedergabe (vermutet)

- WAV-Schreiber `WaveFileFF` (M4): Header 0x0810C994, Format 0x0810CAF8: PCM, 48 000 Hz, `blockAlign = Kanäle x 3`
  -> **24 Bit**, mono oder stereo je nach Aufrufer. Temporäre Datei `-WRITETEMP-.WAV`, Takes `RC*.wav`.
- Transport und Tracks (`trackplay` 0x16D, `trackstarttm` 0x166, `trackendtm` 0x167, `tracktake` 0x165,
  `transplayfrom` 0x183, Punch 0x184/0x185) werden nur auf dem **M4** ausgewertet (u. a. 0x08125040, 0x08126524,
  0x081240C0); der M7 kennt sie nur aus seiner Tabelleninitialisierung 0x08053DE4.
- Kein Loop-/Cycle-Modus gefunden (einzige "Loop"-Strings: WAV-`smpl`-Chunk beim Einlesen). Wiederholen müsste
  ergänzt werden: Sprung des Abspielkopfs ans Bereichsende -> Anfang, mit Vorlauf im Streaming (pcmStreamer,
  288-Block-Cache), damit der Übergang lückenlos ist. Ablauf des Streamings noch nicht untersucht.
- Streaming-Cache im M7 (Objekt angelegt von 0x0805B1A8 über 0x0804BCD8 aus `main`): 288 Blöcke zu 0x26000 Byte,
  bis 0x200 Assets mit je einer Indextabelle Block -> Cache-Slot (0xFFFF = fehlt; +0x2708 je Asset). Klasse mit
  vtable 0x08076BD4; Slot 2 = `read` 0x0805B350(this, ?, int64 Sampleposition r2:r3, Asset-Id [sp+0x40], ...,
  Anzahl [sp+0x4C]) rechnet Block und Offset aus der Position; Lookup 0x0805B210. Die Position führt der Aufrufer
  (noch nicht gefunden, virtueller Aufruf). Wahlfreier Zugriff: ein Loop = Position am Ende zurücksetzen, solange
  die Anfangsblöcke im Cache bleiben bzw. rechtzeitig nachgeladen werden.
- **Aufnahme in der Simulation (emu, 2026-10-26):** Rec-Reiter + Kanal 1 per Touch scharf, PLAY, REC, 3 s, STOP
  -> `Projects/New Project/01-1-00000.wav`: PCM, 2 Kanäle, 48 kHz, 24 Bit (+ `JUNK`-Chunk), 3,013 s.
  `assets.xml`: `<cell row="0" filename=".\01-1-00000.wav" type="asset"><params trackid="0" trackstarttm="1251"
  trackendtm="7036" trackplay="1"/>` -> Zeiten in Ticks, **960 PPQ** (5785 Ticks = 3,013 s bei 120 BPM).
  Pegel/Frequenz der Aufnahme passen nicht zum eingespeisten Sinus (0 dBFS, ~630 Hz statt -6 dB, 440 Hz):
  vermutlich Artefakt der Einspeisung in der Simulation, nicht geprüft.
- **Track-Player = Kanalknoten `vt_bf74`** (process 0x080574EC, Player-Funktion 0x08057B78). Er rendert Clips als
  Stimmen über `FUN_08057A00(player, voice, bus, ?)` (3 Aufrufe 0x08057D1E / 0x08057EF8 / 0x08057F06), die den
  Cache über `vtable[2]` liest (Aufruf 0x08057A92; Cache-Objekt 0x10044270 = SRAM3 0x30044270, vom M4 angelegt
  und gefüllt). Gelesen wird in Blöcken zu 32 Samples ab Asset-Sample 0 (emu).
  Stimme: +0x00 aktiv, +0x01 endet, +0x04 Asset-Id, +0x08 int64 Leseposition (Samples im WAV, += n je Block),
  +0x18 int64 Startverzögerung, +0x20 int64 Restsamples (< 1 -> Ausblenden 0x0805D864, nächster Clip:
  Liste Player +0x98 zu 0x28 Byte, Index +0x5A0, Anzahl +0x598, Start 0x0805779C), +0x28 Rampe.
  **Loop-Ansatz:** wenn die Restsamples ablaufen, Position und Rest auf Loop-Anfang/-Länge zurücksetzen statt
  zu beenden (Wrapper um `FUN_08057A00`); offen: Sprung innerhalb eines 32-Sample-Blocks (sonst bis 0,67 ms Versatz
  je Durchlauf, ohne Drift wenn der Rest mitgezählt wird), Überblendung am Sprung, und ob der M4 die Anfangsblöcke im
  Cache hält (16 Takte Stereo 24 Bit bei 120 BPM ~ 60 Blöcke; Cache 288).
- Projekte enthalten "Presets" (`PresetMgr`, `SessionMgr Load New Preset: %s`, `LoadBank`); was ein Preset-Wechsel
  mit laufenden Tracks macht: offen.

## Looper: Stand und offene Fragen

Gewünscht (Kyrill, 2026-10-25): Loops von 1..16 Takten auf **2 Slots**, **24 Bit stereo**, direkt auf SD
aufnehmen und von SD abspielen, Ausblenden mit Filter. Projektwechsel (= "Preset" bei Kyrill) muss den Loop
**nicht** überleben. Weg: eingebauter Recorder/Player, ergänzt um taktgenaue Länge, Wiederholung und Filter-Fade.

- RAM-Rechnung: frei ~6,4 MB (1,5 + 4,9). 24 Bit mono 3 MB je Slot = 21,8 s (16 Takte nur ab ~176 BPM),
  stereo 10,9 s. Daher SD.
- Einhängepunkt: noch offen; Kandidaten sind die Kanalkette (Bus 0..11 vor dem Mixer `b988`) oder der Mixer selbst.
- Tempo: `globtempo` (0x085); Clock-Quelle/Taktposition im M7 noch nicht gefunden.
- Preset-/Projektwechsel: Ablauf (`SessionMgr`, `PresetMgr`) noch nicht untersucht.
