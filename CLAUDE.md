# Projekt: Blackbox-Mod-Funktionen auf die 1010music Bitbox micro portieren

Arbeitssprache: Deutsch. Antworten konkret, ohne Hedging; Unsicheres klar als "nicht verifiziert" kennzeichnen.

## Ziel
Funktionen aus Justin Joes Blackbox-Mod (Repo `j3threejay/blackbox-mod`, MIT) als eigenen
Patch fuer die Bitbox micro (Firmware 2.3.4, `MICRO.BIN`) bauen. Reihenfolge:
1. Fidelity (Blackbox ersetzt das vorhandene `Interp`-Feld; die Micro hat `Interp:`/`interpqual` auch)
2. 24-dB-Filter (SVF)
3. Munchi (granulares Delay) und Reverb-Typ Room
4. Saturation / Ducker: nur falls sich Hooks finden lassen. Die Micro hat keinen
   `Overdrive`-Parameter und keine `duck*`-Strings.
Nicht portieren (hängen an Mixer, Keys-Screen, Piano Roll der Blackbox): Solo, Chords, Note Conditions.

## Dateien (muessen lokal in firmware/ liegen, werden NIE committet)
- `firmware/MICRO.BIN`  Bitbox micro 2.3.4, 671376 Bytes,
  SHA-256 54ecc3e5c09b9aed7ad8e2d9d1c9f8ee63e06a8351c857304c3ff21fcb9c4365
- `firmware/BLACKBOX.bin`  Blackbox 3.1.9 (der Mod ist darauf gepinnt),
  SHA-256 281ae303d32e5eb52adca7817a648a34e4bd3848017f26673dd9df3905f1341d
  (zum Vergleich lag bisher nur 3.1.8 vor, SHA-256 078847398cdad7663b3cbdcb1edbce49b5d80a2bfbbf4bc9292d88be19d11e7a)

## Gesicherte Fakten (aus bisheriger Analyse)
- Beide Images sind rohe, unverschluesselte Cortex-M7-Images (Thumb-2, Hard-Float FPv5), Ladebasis 0x08040000.
  Der Bootloader liegt NICHT in der Datei. Kein Decodieren noetig.
- Micro: Initial-SP 0x240692A8, Reset 0x08040515. Blackbox: Initial-SP 0x20020000, Reset 0x080403C3.
  => RAM-Layout verschieden; alle RAM-Adressen der Blackbox-Stubs muessen in der Micro neu gefunden werden.
- Gemeinsame Code-Basis (Pfade `matrixsw\BoomboxFramework\...`): ca. 2500 identische Strings,
  ca. 19 % identische 48-Byte-Codefenster (Micro 2.3.4 gegen Blackbox 3.1.8).
- Die Micro hat bereits ein EQ-Objekt (`eqcutoff*`, Biquad-Handler) und einen Master-Kompressor (`globcomp`).
- Recovery Micro: weisse Pfeiltaste rechts beim Einschalten, `MICRO.BIN` im SD-Root. Blackbox: BACK + INFO.

## Referenz-Repos (lokal klonen, siehe setup.sh)
- `repos/blackbox-mod`: src/ (C, DSP), patches/ (Adressen fuer 3.1.9), build.sh, test_*.py (Unicorn-Tests), ghidra/ + findref.py/findbl.py/disasm.py (Hilfsskripte)
- `repos/bitbox-micro-fw-patch`: patch_micro.py (Hooks fuer 2.3.4, bekannte Adressen, Patch-Store im AXI-SRAM,
  Master-EQ und Kompressor schon enthalten). Lies die Kommentare in patch_micro.py als Wissensbasis.

## Methode
1. Hash der Eingabe pruefen, nie auf anderen Dateien arbeiten. Originale nur kopieren, nie veraendern.
2. Strings -> Literal-Pool-Referenzen -> Funktionen (`tools/dis.py`, capstone). Funktionen benennen und in
   `re/symbols.md` mit Adresse, Beleg und Status dokumentieren (`vermutet` / `durch Emulation bestaetigt` / `hardware-verified`).
3. Neuen Code in C schreiben (arm-none-eabi-gcc, `-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16`),
   hinter das Stock-Image anhaengen, Aufrufstellen per Detour umleiten (wie blackbox-mod/patch.py).
4. Jeden Patch-Punkt vor dem Schreiben gegen die erwarteten Stock-Bytes pruefen; Patcher bricht bei Abweichung ab.
5. Neue Funktionen mit Unicorn testen (wie test_*.py im Blackbox-Repo), bevor irgendetwas geflasht wird.
6. Status `hardware-verified` nur, wenn Kyrill es am Geraet gehoert hat. Emulation zaehlt nicht.

## Harte Regeln
- Niemals Firmware-Dateien, gepatchte Images, Disassembly-Listings oder 1010music-Handbuecher committen oder weitergeben.
  `firmware/`, `out/`, `*.bin` stehen in .gitignore. Weitergegeben werden nur Patch-Quellen, Adresslisten und Build-Skripte.
- Kein Flashen ohne Kyrills ausdrueckliche Freigabe. Vor jedem Flash: Stock-Image sichern, Verifikation (Null-Repack,
  alle Aenderungen innerhalb registrierter Bereiche, Detours springen zurueck) muss gruen sein.
- Splash/Version muss ein modifiziertes Image kennzeichnen (wie `community fw` im Micro-Patcher).
- Nicht erfinden: Bei unbekannten Adressen, Strukturen oder Verhalten nachschauen oder als offen markieren.

## Erste Aufgabe
Die Funktion hinter dem String `Interp:` in `firmware/MICRO.BIN` lokalisieren, ihre Entsprechung in der Blackbox
(Mod-Patch `patches/` fuer Fidelity) finden und beschreiben, was zum Portieren fehlt.
