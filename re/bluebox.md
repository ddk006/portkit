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
- **Track-Player = erster Kanalknoten `vt_bfdc`** (vtable 0x0806BFDC, process 0x08057B70; korrigiert in L7, vorher
  hier `vt_bf74`, dessen Prozess 0x080574EC nur die Parameter-Events der Spur liest). Er rendert Clips als
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

- **Prototyp `loop` (2026-10-26, `bluebox/src/loop_m7.c`):** Wrapper um `FUN_08057A00` an den drei Aufrufstellen.
  Unicorn (`tests/test_bluebox_loop.py`): 8/8. Renode (480/240 MIPS, sauberer 440-Hz-Sinus als Clip, 3,013 s):
  Ausgang (Slots 2/3) läuft bis zum Ende des Mitschnitts weiter, stock endet nach 3,1 s; Sprünge der Leseposition
  144597 -> 4 und 144612 -> 19 (Clip 144625 Samples), Abstand der Sprünge 3,013 s (keine Drift); an jedem Sprung ein
  Knackser (Signalsprung 185x / 480x über der Sinuskrümmung). SRAM4 unter 0x3800F000 blieb während des Laufs
  unberührt (Canary). Nicht am Gerät gehört.
- **Überblendung (2026-10-26, Build 1.5.1L2):** `loop_read` ersetzt den Cache-Lesezugriff (vtable 0x08076BD4 Slot 2,
  `read(this, fn, int64 pos, asset, float *l, float *r, n)`, Float-Samples, != 0 = ok; Cache-Blöcke 0x9800 Samples
  je Kanal). Nach einem Sprung werden die ersten 256 Samples ab der Landestelle aus Kopf x(p) (einblenden) und
  Fortsetzung hinter dem Loop-Ende x(p + len) (ausblenden) gemischt, gleiche Leistung. Scheitert der Lesezugriff des
  letzten Blocks vor dem Sprung am Dateiende, liest `loop_read` den Teil bis zum Loop-Ende einzeln (sonst spielt die
  Stimme den ganzen Block still). Unicorn 13/13. Renode (Sinus-Clip mit 1 s Audio hinter dem Clip-Ende): kein
  Knackser mehr (max. 2,8x der Sinuskrümmung, vorher 185x/480x), Pegel wie stock (0,310 beide). Offen: ein früherer
  Lauf mit anderem Kartenabbild zeigte 0,62 (Ursache nicht geklärt).
- **Taktlänge (2026-10-26, Build 1.5.1L3):** Transport = `*a3` (viertes Argument der Stimmen-Funktion, wie an
  0x0805779C weitergereicht): +0x18 float Tempo, +0x30 int64 Position in Ticks (emu: 120.000, +1920 Ticks/s).
  Umrechnung 0x0805AB80: Samples = Ticks / 960 / BPM x 60 x 48000 (Konstanten 960.0 / 60.0 bei 0x0805ABF8).
  Clip-Eintrag (r2 der Stimmen-Start-Funktion): +0x08 Asset, +0x10 int64 Start (Ticks), +0x20 int64 Versatz in der
  Datei (Ticks). Loop: erster Taktstrich (3840 Ticks, 4/4 angenommen) ab der Stelle, an der der Clip auf der
  Zeitleiste spielt, `bars` Takte lang; Sprunglängen aus einem 48.16-Festkomma-Akkumulator. Unicorn 18/18
  (120 BPM 2 Takte: Sprünge 192000, Landung im Block des Taktstrichs 64725; 133 BPM 3 Takte: Sprünge 259849/259850,
  Summe nach 6 Durchläufen 1559097 bei exakt 1559097,74). Renode (Clip 1251..8931 Ticks, bars = 1 per SRAM4):
  120 BPM Start 64725, Länge 96000, 3 Durchläufe in 8 s; 133 BPM Start 58399 (erwartet 58398, Rundung der Tickposition),
  Länge 86617 (exakt 86616,54), 3 Durchläufe. Offen: Taktart (nur 4/4), Tempowechsel während des Loops.
- **LOOP/BARS pro Spur (2026-10-26, Build 1.5.1L4):** Kanal-Panel = Seitenobjekt `FUN_08133470` mit festen Widgets
  (8 Regler à 0x3D0 ab +0xC398, 5 Buttons à 0x1D4 ab +0xE298; Label + Parameter-Id, z. B. REC 0x162, MUTE 0x160);
  Mixerseite mit Reitern `FUN_0812F398` (Listen 0x0814E280 / 0x0814E290). Kanal-Parametermenge = `FUN_0812060C`
  Fall 1 (u. a. 0x004, 0x003, 0x063, 0xDA, 0xDB, 0x15D..0x163, 0x16D, EQ 0x119..0x12F, Ducker 0x1F..0x2A).
  Freie Ids (in keiner Tabelle, nirgends verglichen oder geladen): 0x102..0x105, 0x107..0x10A; belegt: 0x103 LOOP
  (Toggle, `chloop`), 0x104 BARS (1..16, `chloopbars`, Standard 4). Events: Postfach-Eintrag +0x08 = Ziel-Queue
  (= Spur), +0x0C Id, +0x10 Wert; der Player-Prozess prüft +8 gegen seine Queue-Id (+0x18).
  Unicorn 24/24. Renode: M4 definiert beide Parameter; LOOP = 1 / BARS = 1 per Postfach für Spur 0 -> Loop ab 64725,
  96000 Samples; LOOP = 0 -> Signal endet sofort (6,55 s statt Loop bis 9 s). Der M4 schickt die Standardwerte der
  neuen Kanal-Parameter selbst an den M7 (BARS = 4 für Spur 1 gesehen). Noch keine Widgets im Panel.
- **FILTER/FADE (Build 1.5.1L5):** 0x105 FILTER (Typ 9 wie Pan, -1000..1000), 0x107 FADE (0..16 Takte). Filter
  (TPT-SVF, Q 0,8) und Lautstärke im Lesepfad nach der Überblendung; `loop_voice` setzt vor der Stock-Stimme die
  Werte des Blocks. Zustand mit Layout-Version im Magic ("LOO5"). Unicorn 30/30. Renode (440-Hz-Clip auf Spur 1,
  1-kHz-Ton am Eingang): FILTER -850 senkt den Loop um 19 dB, FADE 1 Takt blendet über 2 s weich aus, Slot danach
  frei; der Eingang bleibt die ganze Zeit hörbar (0,79), nach dem Ende des Clips 1,10: stock senkt den Eingang
  ~3 dB, solange der Clip der Spur spielt (Ursache nicht untersucht).
- **Seiten und Tasten (emu):** Matrix-Codes 0xF9 / Param 0..7 = Seitentasten: 0 Mixer, 1 **Track**, 2 Ausgänge,
  3 EQ, 4 FX, 5 Transport (Play From / Punch In / Punch Out), 6 Projekt, 7 Einstellungen; E0 (Zeile 0, Spalte 0):
  Spuranzahl-Menü. Track-Seite = Seitenobjekt eingebettet im App-Objekt (App + 0xCFB70, nächste Seite + 0xDE810,
  vtable 0x0814E5D4, Konstruktor 0x08133470 aus 0x08135B34). Regler (vtable 0x0814DA10) +0xC398 + i*0x3D0: Rechteck
  +4 (x, y von unten, w, h; int32), versteckt +0x30, Parameter-Id +0x94, Label +0xF8; 0 VOL, 1 PAN, 2 GAIN, 3 FX1,
  4 FX2, 5 CUE, 6 OUT3&4, 7 (0x15F, nur in 3&4-Mono). Buttons (vtable 0x0814D9C4) +0xE298 + i*0x1D4: Id +0xFA;
  0 REC, 1 MUTE, 2 SOLO, 3 CUE-Schalter (versteckt), 4 SIDE CHAIN (0x2B). Kein freies Widget.
- **Loop-Ansicht (Build 1.5.1L6, emu):** Tasten: E1 = B/Info (schaltet auf der Track-Seite den Block), E0 = Eingangs-
  zuordnung (Raster 1..12). Tasten-Handler 0x081354EC: Nachricht 0x8C (Track-Seite mit Block +0x0C) ->
  showPage(pm, page) 0x0813AA1C (bl @0x08135718; aktuelle Seite pm + 0x34) und setBlock(page, block) 0x08133884
  (bl @0x08135720; Feld page + 0xEBBC; Track-Taste: Block 0, B: der andere Block). Nachrichten-Handler der Seite
  (vtable 0x0814E5D4 Slot 13 = 0x081342AC): 0x32 Encoder (Index +0x0C, Delta +0x10; Zuordnung je Block fest),
  0x63/0x66/0x67/0x141 Update (0x081339B0 -> 0x081339B8: je Regler get 0x08122580, bind 0x08128BD0, fmt 0x08124958,
  setText 0x08128FC8; Parameterspeicher 0x30018010), 0xD4 Layout. Widgets: setRect vtable +0x20, setHidden +0x2C,
  addChild 0x0813B994 (verkettete Liste, Seite +0x18/+0x1C). Regler: Wert +0x84, Id +0x94, Typ +0x96, Spur +0x32,
  drehen 0x08128D9C -> Nachricht 0x6E {+8 Spur, +0xC Id, +0x10 Wert} an den Elter (0x0813BA64). Button: Zustand
  +0x96, Spur +0xF8, Id +0xFA, +0xFC = senden UND Anzeige invertiert (Leuchten = 0); Tipp = vtable Slot 6 0x081280EC,
  setOn 0x081280B4. Unser LOOP-Button: eigene vtable-Kopie, Tipp schickt 0x6E selbst. Seite eingebettet im App-
  Objekt -> eigene Widgets in SRAM4 0x3800A000. B schaltet Block 1 -> Block 2 -> Loop -> Block 1; in der Loop-Ansicht
  Regler in Block 2s Rahmen (Encoder 1 FILTER, 2 BARS, 3 FADE), LOOP rechts. LOOP-Tipp setzt im M7 den Slot (emu).
  Fix: LOOP aus mit FADE, während nichts spielt, gibt den Slot sofort frei (vorher blieb er belegt).
- **Aufnahme, die nach BARS Takten endet und sofort loopt (Build 1.5.1L7, emu):**
  - REC = App-Nachricht 0x69 (STOP 0x6C, kommt auch direkt nach REC); Codes < 0x7B -> 0x0813AA90 (bl @0x08135B24).
    Take beenden = 0x081272CC(store, 0) (Record aus): läuft ein Take (store + 0x743B), setzt 0x08127150 sein Ende,
    stellt das Schließen der Datei als Auftrag 0xC in die Recorder-Queue (0x0810A8FC) und schickt den Clip an den M7
    (0x08125040). Der Transport läuft weiter; stock beendet Takes nur bei STOP.
  - **Punch In/Out per 0x08126524 bei laufendem Transport geht nicht:** der Take wurde angemeldet (`assets.xml`,
    Start = REC-Zeitpunkt statt Punch In), die WAV aber nie geschrieben (keine `_01-1-…` Temp-Datei, Karte bis auf
    die XML unverändert). Stock und L7 ohne LOOP schreiben die WAV (Gegenprobe). Daher setzt L7 keinen Punch mehr:
    Aufnahme ab REC wie stock, `loop_tick` (am Redraw-Check bl @0x081351A6) schaltet Record bei
    nächster Taktlinie (>= 1/16 nach REC) + BARS Takte + 1/16 aus.
  - Länge im Cache: 0x0810C608(cache, asset, rate, ?, int64 Länge) setzt Slot +0x1B20 (Aufrufer 0x0810B4C6,
    0x0810B940 (8f6: Länge 0), 0x0810BAEC (9e4: Datengröße aus dem WAV-Header / Blockgröße)). Nach dem Take meldet der
    Streamer die Datei neu an (Zuordnung kurz 0xFFFF). Getter 0x0805AC88 liefert -1 ohne Zuordnung.
  - **Player-Flags:** +0x94 = eine Stimme läuft (gesetzt vom Start 0x0805779C; nur dann rendert/plant er), +0x95 =
    Wiedergabe-Status (Parameter 0x16D über 0x39-Event), +0x96 = Rec Enable (0x162). Nachricht 0x57 (bei PLAY) plant
    mit +0x95 die Clips: 0x08057900(player, -, ctx) -> Start 0x0805779C (zweites Argument wird nicht gelesen).
    Ein Clip, der auf einer stillen Spur bei laufendem Transport dazukommt, startet stock erst beim nächsten PLAY.
    Event-Ids im Player (0x39, Sprungtabelle 0x08057E04): 0x162 -> +0x96, 0x166/0x167/0x168/0x16B/0x16F Clip-Felder,
    0x16C Anzahl, 0x16D -> +0x95.
  - **Rec Enable schaltet die Clips der Spur stumm:** ist +0x96 gesetzt und nimmt der Transport nicht auf
    (Transport +0x38 = 0), nimmt der Render-Zweig @0x08057CA4 -> 0x08057E50 den Eingang statt der Clips (Renode:
    Loop lief, Ausgang 0,000; nach Rec Enable aus 0,70).
  - L7: `autostart` verschiebt den Clip (Start jetzt + 1/16, Versatz = Loop-Phase) und ruft die Planung 0x08057900
    selbst auf, bis 200 Blöcke lang wiederholt, bis eine Stimme läuft; der M4 schaltet Rec Enable der Loop-Spuren
    nach dem Take per 0x6E an 0x08126E8C aus.
  - **Stream-Fehler in der Kopfzeile:** 0x08122C08 -> Statusfeld +0x18 = Cache +0x362C + 0x3000E634; 1 -> "REC ERROR",
    > 1 -> "ERROR %d" (statt freiem Speicher). Endet ein Take bei laufendem Transport, liest der Mixer `b988`
    (0x08050564 -> 0x0805012C -> 0x0805AD1A, liest die aufnehmende Spur an der Schreibposition) ~8 Ticks lang ins
    Leere: 7 Fehler (Ticks 8024..8032 nach Take-Ende 7920, Zähler 0x0805B05C). Stock mit STOP: keine. L7 zählt
    Fehler in den ersten 480 Ticks nach dem Take zurück. Hörbar vermutlich ~5 ms Lücke im Mithören am Take-Ende
    (nicht verifiziert).
  - Renode (frische Karte, 120 BPM, BARS 1, 440-Hz-Eingang, REC bei Tick ~1690, Eingang ab 4,4 s still): WAV
    958200 Byte (24 Bit stereo), Take-Ende 7920, Loop ab Taktlinie 3840 mit 95968/96000 Samples, Ausgang ab Take-Ende
    durchgehend 0,70 (Loop), Kopfzeile ohne Fehler.
- **Looper L8, unabhängig vom Spur-Recorder (2026-10-28, emu):** Rückmeldung zu L7 am Gerät (Kyrill, 2026-10-27):
  bootet und nimmt auf; Regler ohne Werte und zu schnell, kein Status sichtbar, Loop an den Recorder gebunden.
  Gewünscht: Quelle = Ausgang der Spur, Start an der nächsten Taktlinie, Datei auf SD (RAM frei halten), Aufnahme von
  Spuren / Master muss weiterlaufen, Loop darf auf der Master-Aufnahme hörbar sein, Pegel wie die Spur.
  - Stream-Cache (Objekt 0x10044270, vtable 0x08076BD4): [0] write(cache, asset, l, r, n) 0x0805AD08 (schreibt an
    die Schreibposition des Assets, Slot +0x1B18; Aufrufer bei der Aufnahme: Mixer `b988` über 0x0805012C);
    [2] read 0x0805B350; [4] 0x0805B7BC -> 0x0805B210(cache, asset, block): Block-Anforderung an den M4 (Ring
    +0x2B0C, 128 Einträge {Block, Typ 4, Asset}; fordert Block b..b+3 an, berührt nur den LRU-Zähler, wenn der Block
    schon in der Tabelle steht); [5] **length(int64 *out, cache, asset)** 0x0805AC88 (Ergebnis über das erste
    Argument!). Blocktabelle je Datei: Zeiger bei cache + 0x352C + 4 * Datei-Slot, u16-Einträge (b + 0x400 für
    b < 32, sonst b & 0x3FF); Deskriptoren zu 24 Byte bei cache + 24 * e: +4 Float-Daten, +8 Block, +0x10 Samples,
    +0x14 Asset, +0x16 rechter Kanal, +0x18 in Benutzung, +0x19 wird geladen. Blockgröße 0x9800 Samples je Kanal.
    Die Stock-Leseroutine kopiert ohne Prüfung des Datenzeigers.
  - M4-Stream-Manager (Objekt 0x3000B778, erstes Wort = Cache): Aufträge über 0x0810A630 (Kommando 8 - Typ: 7 =
    Datei lesen, Handler 0x0810B9E4), 0x0810A840 (10 - Arg5: 9 = Datei schreiben, Handler 0x0810B8F6, gibt das Asset
    vorher frei 0x0810B8B0), 0x0810A8FC (12 = schließen, Handler 0x0810B380: Header, Länge 0x0810C608 in den
    Cache), 0x0810A6E8 (29 = Datei kopieren/umbenennen), Verteiler 0x0810BC0C. Stock-Take (Spur): beim Scharfschalten
    openW(asset, "Projects\<Projekt>\01-1-00000.wav", 2), ab REC M7-Writes, bei STOP close + openR, dann openW
    des nächsten Takes.
  - Mixer-Kanalpfad 0x0805012C (für Kanäle mit Aufnahme-Flags) ruft den Cache über seine vtable auf.
  - Fehler, die die Simulation gezeigt hat (alle behoben): (1) 1 KB Stack-Rahmen in `loop_process` (läuft je Spur im
    Audio-Interrupt) überschrieb Firmware-Daten, eine Stock-FFT las danach aus den Ethernet-Registern -> Arbeitspuffer
    in SRAM4 0x38008000. (2) Längenabfrage als length(cache, asset) aufgerufen: schrieb -1 / die Länge über den
    vtable-Zeiger und Deskriptor 0 des Caches -> scheinbar kaputter Block 0 (Daten -1, Absturz der Stock-Leseroutine
    beim Sprung zum Loop-Anfang) und Absturz im Mixer-Aufnahmepfad, sobald der Transport aufnahm (vtable-Watchpoint:
    Schreiber 0x0805AC98, aus `loop_process`). Ein zwischenzeitlicher Versuch, den "verwaisten" Deskriptor 0
    freizugeben, legte das ganze Audio lahm (er war nur von (2) überschrieben).
  - Renode L8 (frische Karte, 120 BPM, BARS 1): LOOP bei Tick 1460, Aufnahme ab 3840, close 7764, Loop ab 7790
    durchgehend 0,70 am Ausgang; dasselbe, während Spur 3 per REC aufnimmt (Take geschrieben, Loop hörbar).
    Eine Spur, die selbst scharf geschaltet ist und aufnimmt, gibt den Loop nicht aus (anderer Signalweg, offen).
  - Regler: Typ-1-Regler (ganze Zahl) bewegen sich einen Wert je Encoder-Zähler (0x0812877C: Wert = min + Position),
    VOL braucht ~2000 Zähler; eigene Schritte (BARS / FADE 96 Zähler, FILTER 1 % je 8). Die Stock-Drehung bewegte
    FILTER (Typ 9) in der Simulation nicht. Werte in den Beschriftungen (snprintf 0x08146156).
- **L8 am Gerät (Kyrill, 2026-11-01): Installation geht, Bildschirm bleibt beim Boot schwarz.** L7 bootete.
  - M7-MPU (0x0805D974, HAL_MPU_ConfigRegion 0x0805E318, Enable mit PRIVDEFENA): Regionen 0 DTCM 128 KB (C/B),
    1 0x24000000 256 KB (C/B), 2 0x24040000 64 KB (C/B), 3..5 0x24050000 / 0x24060000 / 0x24070000 je 64 KB
    nicht cachebar, 6 0x30000000 512 KB (C/B), 7 Flash 2 MB, 8 SDRAM 0xC0000000 64 MB (C/B), 9 0x40000000 512 MB
    (Device), 10 0x60000000, 11 0x1FF0F000, 12 0x10040000 64 KB (shareable: der Austausch der Stock-Firmware).
    **SRAM4 0x38000000 liegt in keiner Region**: für den M7 Standard-Attribut, Write-Back-Cache (D-Cache an,
    0x0805DD52). M7 und M4 sähen dort ihre Schreibzugriffe nicht zuverlässig (der M4 hat keinen Cache).
  - Wahrscheinliche Ursache des schwarzen Bildschirms (nicht am Gerät bewiesen): `loop_tick` (UI-Schleife, ab dem
    Start) las in L8 die Widget-Daten in SRAM4 ohne Gültigkeitsprüfung; nach dem Einschalten steht dort Zufall ->
    Müllzeiger. Renode startet mit genulltem SRAM4 und toleriert ungültige Zeiger (Boot mit 0xA5 in SRAM4 lief).
  - L9: alle M4-Einstiege prüfen Magic + `built`; der Konstruktor der Track-Seite (`loop_track_ctor`, vor dem
    Stock-Konstruktor) setzt UI und gemeinsamen Zustand ungültig. Erster Versuch in `loop_defs` war falsch: die
    Parametertabelle wird in Renode *nach* der Track-Seite aufgebaut, der Reset machte die Loop-Ansicht ungültig
    (Ansicht fehlte, Slots IDLE). Offen: nach einem Warmstart läge bis zum Konstruktor ein gültiges altes Magic in
    SRAM4 (nur Zeitfenster vor dem Aufbau der Track-Seite); der M7 richtet beim ersten Aufruf MPU-Region 13 für SRAM4 ein (normal, nicht
    cachebar, TEX 1 C 0 B 0) und schreibt vorher gecachte Zeilen zurück (DCCIMVAC).
- RAM-Rechnung: frei ~6,4 MB (1,5 + 4,9). 24 Bit mono 3 MB je Slot = 21,8 s (16 Takte nur ab ~176 BPM),
  stereo 10,9 s. Daher SD.
- Einhängepunkt: noch offen; Kandidaten sind die Kanalkette (Bus 0..11 vor dem Mixer `b988`) oder der Mixer selbst.
- Tempo: `globtempo` (0x085); Clock-Quelle/Taktposition im M7 noch nicht gefunden.
- Preset-/Projektwechsel: Ablauf (`SessionMgr`, `PresetMgr`) noch nicht untersucht.

### Schwarzer Bildschirm L8/L9: Ursache (hardware-bestätigt per Bisektion, 2026-11-01)
- Testbuilds am Gerät: T3 (nur M7) bootet, T2 (nur M4) schwarz; T4 (nur `loop_tick`) bootet, T5 (M4 ohne
  `loop_tick`: Parameter, Loop-Ansicht) schwarz.
- `struct loop_ui` bekam in L8 `acc[3]`, `text[16]`, `lit` (1 Byte) vor den Widget-Objekten: Knobs ab Offset 61,
  Button ab 2989 (L7: 32 / 2960). Die Stock-Konstruktoren schreiben mit STRD/STM; unausgerichtet -> UsageFault auf
  dem Cortex-M4. Renode toleriert unausgerichtete Zugriffe, Unicorn auch.
- Fix LA: `aligned(8)` und `_Static_assert` auf die Offsets. Ob LA bootet: noch nicht am Gerät.

