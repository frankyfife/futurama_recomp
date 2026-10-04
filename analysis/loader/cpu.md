# CPU-Ebene: FS-Segment, privilegierte Befehle, Hardwarezugriffe (retail_eu)

Ziel: alles finden, was im Original-XBE (`orig\retail_eu\default.xbe`) **nicht unverändert in einem 32-Bit-Windows-User-Mode-Prozess** läuft, wenn die Sektionen an ihren Originaladressen gemappt werden.

Maschinenlesbare Ergebnisse (alle in `D:\FuturamaDecomp\analysis\loader\`):

| Datei | Inhalt |
|---|---|
| `fs_sites.tsv` | alle 5206 echten fs:-Zugriffe (Spalten: `addr, bytes, ins, offset, func, section, seg, based, memop, size, fstart, inside, cls, lib`) |
| `cpu_priv_sites.tsv` | privilegierte und Sonderbefehle in echtem Code (inkl. int3, rdtsc, cpuid, SSE/MMX) |
| `cpu_hw_refs.tsv` | Hardware-Adressen in echtem Code (MMIO-Zugriffe, I/O-Ports, `or 0x80000000`, Kernel-Adressen; reine Konstanten herausgefiltert) |

Skripte (alle in `scripts\`, reproduzierbar, nur lesend):

| Skript | Zweck |
|---|---|
| `cpu_rd.py` | Recursive-Descent-Disassembler (Seeds: 31337 Ghidra-Funktionen, Entry, Codezeiger aus Daten/Immediates; folgt Sprüngen, Calls und Switch-Tabellen) |
| `cpu_gaps.py` | nicht erreichte Bytes der Codesektionen, klassifiziert als `pad` / `ascii` / `codelike` / `data` |
| `cpu_scan.py` | Hauptscan, schreibt die drei TSV-Dateien (`--all` nimmt auch Pseudo-Befehle aus Datenbereichen auf) |
| `cpu_fsctx.py` | Kontext jedes fs:-Zugriffs mit Offset ungleich 0, TLS-Verzeichnis und Referenzen auf den TLS-Index |
| `cpu_kthunks.py` | Kernel-Thunk-Slot → Importname, aus den Ghidra-Namen im Decompilat abgeleitet (67/111 aufgelöst, kein Ordinal-Wissen aus dem Gedächtnis) |
| `cpu_sections.py` | Zugriffe auf den XBE-Header und die Section-API |
| `cpu_smc.py` | Suche nach selbstmodifizierendem Code |
| `cpu_sehhandlers.py` | welche SEH-Handler die fs:[0]-Frames eintragen |
| `cpu_datascan.py` | MMIO-Adressen als Dwords in `.rdata`/`.data` |

---

## 0. Kurzfassung

1. **fs:-Zugriffe: 5206 echte Stellen, nur 6 verschiedene Offsets.**
   - `fs:[0]` kommt 5168-mal vor: 4878 Stellen in den Körpern von 1419 Ghidra-Funktionen, 290 Stellen hinter dem Ende eines (zu kurz bemessenen) Ghidra-Körpers. In der Spalte `func` ergibt das 1486 verschiedene Werte (1419 Funktionsnamen + 67 `?(after …)`-Werte); 1486 ist also keine Funktionsanzahl (korrigiert, vorher „1417 + 69“). Das ist die SEH-Kette, die mit `TEB.NtTib.ExceptionList` übereinstimmt. Diese Stellen bleiben ungepatcht.
   - Nur **38 Stellen** greifen auf andere Offsets zu: `fs:[4]` ×11, `fs:[0x20]` ×8, `fs:[0x24]` ×8, `fs:[0x28]` ×9, `fs:[0x58]` ×2. Sie liegen in XAPILIB, LIBCMT (`_getptd`), DSOUND und D3D sowie an 2 inline-expandierten Stellen im Spielcode (Fiber-Abfrage).
   - Es gibt kein `fs:[0x18]`, `fs:[0x2c]` oder `fs:[0x34]`, keinen gs:-Zugriff und kein Laden von Segmentregistern.
2. **TLS:** Das Spiel nutzt keine Windows-artige TLS über `fs:[0x2c]`.
   - Der Zugriff läuft über `[ [fs:4] + _tls_index*4 ]`. `_tls_index` (0x2f3d18) setzt der Entry-Code bei 0xf851b–0xf852f zur Laufzeit auf **-9**, also `-(TlsDataSize/4)` mit TlsDataSize = 0x24.
   - Der TLS-Block (0x20 Byte, nur Zero-Fill) gehört XAPI/LIBCMT: +4 LastError, +8 `_tiddata*`, +0xC aktuelle Fiber, +0x10 Fiber-Struktur des konvertierten Threads.
3. **Empfohlene Strategie: Displacement-Retarget.** Bei allen 38 Stellen wird nur das disp32 auf ein eigenes `TEB.TlsSlots[k]` (`fs:[0xE10+4k]`) umgeschrieben.
   - Die Befehlslänge bleibt gleich, es gibt keine Trampoline, die Semantik bleibt pro Thread erhalten.
   - Dafür braucht es 5 TlsAlloc-Slots: TLS-Anker, KTHREAD, KPRCB, IRQL, DpcActive.
4. **Privilegierte Befehle in echtem Code: nur 9**, und alle in Bibliotheken, die ohnehin ersetzt werden:
   - D3D: `wbinvd`, 2×`in`, 1×`out` (Port 0x80C0)
   - XAPI-Kernel-Patcher und XPP: je ein `cli`/`sti`-Paar
   - `OutputDebugStringA`: `int 0x2d`
   - Dazu nicht privilegiert, aber relevant: 2×`rdtsc` (eines davon in `QueryPerformanceCounter`, wo die Frequenz fest auf **733 333 333 Hz** steht) und 3×`cpuid` im Spielcode (CPU-Erkennung ohne Folgen).
5. **Hardware-MMIO steckt ausschließlich in D3D, DSOUND und XPP**, dazu der XAPI-Kernel-Patcher `FUN_000f9804`:
   - APU 0xFE80xxxx: 266 Speicheroperanden in DSOUND (dazu 4 MMIO-Immediates), 1 in XPP
   - AC97 0xFEC00xxx: 22 in DSOUND
   - NV2A 0xFD000000: 8 in D3D
   - OHCI 0xFED00000: 1 in XPP
   - Physisches Fenster 0x80000000: 2 Schreibzugriffe in D3D
   - **Spielcode und NetImmerse greifen nicht auf Hardware zu.** Die 8 `or reg,0x80000000` im Spielcode sind Block-Flags des Speichermanagers, keine Adressen.
6. **Kein selbstmodifizierender Code**: kein absoluter Schreibzugriff in `.text` oder auf Befehlsbytes. Absolute Schreibzugriffe in D3D/DSOUND/XMV/XPP treffen deren eigene Daten- bzw. BSS-Bereiche (siehe Abschnitt 5).
   - Section-Loading betrifft nur `$$XTIMAGE` (über `XapiMapLetterToDirectory` → `XeLoadSection`, Handle = Zeiger auf den XBE-Section-Header bei 0x1052c).
   - `DOLBY` ist DSP56300-Mikrocode, kein x86-Code. DSOUND kopiert ihn in den APU-DSP.
   - Der XBE-Header muss an 0x10000 gemappt **und beschreibbar** sein (der Entry schreibt bei 0xf84f3 ins Zertifikat).
   - Nicht nur `.data` hat BSS: auch D3D (0x243890–0x246cf8), DSOUND (0x26410c–0x264354) und XMV (0x28bebc–0x28becc) haben `VirtualSize > RawSize` und schreiben dorthin (Nachtrag der Verifikation).
7. **Nur zwei XBE-Threads** (Nachtrag der Verifikation): `CreateThread` hat genau einen Aufrufer (Entry 0xf8531, Main-Thread 0xf8469, Default-Stack 0x10000), `CreateFiber` genau einen (0x13f473, Stack 0x4000). Alles Weitere läuft in HLE-DPC/-Timer-Kontexten.

---

## 1. Methode

- **Recursive Descent** (`cpu_rd.py`) erreicht 664 613 Befehle. Seeds sind alle Ghidra-Funktionen, 2636 Codezeiger aus `.rdata`/`.data` und 727 aus Immediates (Nachlauf der Verifikation; vorher 2635/728); Zeiger auf Strings werden verworfen.

  | Sektion | erreicht (Befehlsbytes) | Größe (raw) | Rest |
  |---|---|---|---|
  | .text | 1 976 807 | 2 241 216 | 220 089 Padding, 17 291 code-artig, 21 783 Daten; die übrigen 5 246 B sind von `cpu_rd` als Switch-Sprungtabellen markiert |
  | D3D | 54 548 | 62 928 | u. a. Strings/Tabellen 0x2421a5–0x243890 |
  | DSOUND | 40 873 | 119 820 | 78 KB Datentabellen 0x250ee9–0x26410c |
  | XMV | 32 956 | 162 652 | 126 KB 16-Bit-Tabellen 0x26c5f0–0x28bebc |
  | XPP | 28 738 | 30 332 | |

- **Lücken** (`cpu_gaps.py`): Nicht erreichte Bereiche werden linear disassembliert und klassifiziert.
  - `gap-codelike` = dekodiert vollständig und enthält ein `ret`, z. B. Funktionen, die Ghidra übersehen hat. Beispiele: 0xf8469 (Main-Thread-Routine), 0x1d880.
  - `gap-data` = Daten. Treffer aus `gap-data` sind Pseudo-Befehle. Sie stehen nicht in den TSV-Dateien, `cpu_scan.py --all` erzeugt sie: 409 fs/gs-„Treffer“ und z. B. 127×`in`, 84×`hlt`, alle in Tabellen.
- **Nicht als x86 disassembliert:**
  - `.rdata` (Flags 0x06) und `.data` (Flags 0x07) tragen zwar das Executable-Bit, sind aber Daten. Ein naiver Sweep liefert dort 2755 bzw. 173 falsche fs:-Treffer.
  - `DOLBY` (Flags 0x26): Alle 7259 Dwords haben ein Nullbyte an Position VA%4 = 3. Das sind 24-Bit-Wörter, also DSP56300-Code. `FUN_00250d79` kopiert 0x310740 (800 B), 0x310a60 und sechs Blöcke aus `FUN_00250bc9` (0x311060 … 0x317538) mit `FUN_0024f537` in den APU-DSP.
- **Gegenprobe mit den Ghidra-Grenzen:** Die Spalte `inside` in `fs_sites.tsv` zeigt, ob die Stelle im Body der vorangehenden Ghidra-Funktion liegt; sonst steht `func = ?(after X)`.
  - Ein Scan nur über Ghidra-Funktionskörper (vgl. `scripts\startup_fsscan.out`) findet 29 statt 38 Nicht-Null-Stellen.
  - Die 9 fehlenden liegen in Code, den Ghidra nicht als eigene Funktion erkannt hat: Main-Thread 0xf8469, `XapiThreadStartup` 0xf99da, `_freeptd` 0x169352 (Name Vermutung nach Verhalten, siehe 2.3), Fiber-Startroutine 0x17086b, Rest von `FUN_000f5c77`.

---

## 2. FS-Segment

### 2.1 Überblick nach Offset

| Offset | Anzahl | Encoding(s) | Xbox-Bedeutung (KPCR) | Windows-TEB32 an diesem Offset | Folge ohne Patch |
|---|---|---|---|---|---|
| `0x0` | 5168 | `64 89 0D 00000000` ×2163, `64 A1 00000000` ×1495, `64 89 25 00000000` ×1493, `64 A3` ×6, `64 FF 35` ×2, `64 8F 05` ×2, Rest ×7 | NtTib.ExceptionList | NtTib.ExceptionList | **kompatibel** |
| `0x4` | 11 | `64 8B 0D 04000000` ×9, `64 8B 3D 04000000` ×2 | NtTib.StackBase, als **TLS-Anker** benutzt (`[StackBase-0x24]` = Zeiger auf TLS-Block) | NtTib.StackBase des Windows-Stacks | liest/schreibt die obersten Stackbytes des Windows-Threads: LastError-Müll, Stack-Korruption |
| `0x20` | 8 | `64 A1 20000000` | `Prcb` (Zeiger auf KPRCB). Danach wird immer `[Prcb+0x250]` gelesen (Debug-Monitor-Hook, Vermutung) | ClientId.UniqueProcess (PID) | Zugriff auf `[PID+0x250]` → Access Violation |
| `0x24` | 8 | `64 0F B6 05 24000000` (Byte) | `Irql` | ClientId.UniqueThread (Low-Byte der TID) | zufälliger IRQL: `_getptd` ruft bei ≥2 `KeBugCheck`, DSOUND sperrt nicht mehr |
| `0x28` | 9 | `64 A1 28000000` | `PrcbData.CurrentThread` (KTHREAD*) | ActiveRpcHandle (meist 0) | `[0+0x28]`, `[0+0x12c]` → Access Violation |
| `0x58` | 2 | `64 A1 58000000` | `PrcbData+0x30`, gelesen als „läuft gerade in einem DPC“ (DpcRoutineActive, Vermutung) | in TEB32.User32Reserved (meist 0) | DSOUND überspringt `KeSaveFloatingPointState`, meist harmlos, aber undefiniert |

Die KPCR-Feldnamen stammen aus öffentlichen xboxkrnl-Rekonstruktionen (**Vermutung**). Belegt ist jeweils das Verhalten:

- `fs:[0x24]` wird vor `KfRaiseIrql` (ord 160) mit 2 verglichen (0x246d37ff.).
- `[fs:[0x28]]+0x12c` ist der Rückgabewert von `GetCurrentThreadId` (0xf9ae0).
- `fs:[0x58]` schaltet `KeSaveFloatingPointState`/`KeRestoreFloatingPointState` (ord 142/139) zu (0x246d7a/0x246d9f).

Die TEB32-Offsets sind das öffentlich dokumentierte x86-TEB-Layout. Der Host sollte sie beim Start selbst prüfen (siehe 2.6).

`fs:[0]` nach Bibliothek (Spalte `lib`): game/unknown 4202 (Spielcode **plus** alle nicht per Name erkannten Bibliotheksfunktionen, z. B. unbenannte NetImmerse-Funktionen), NetImmerse 882 (nur Funktionen mit Namen `Ni*`), C++-Init-Stubs 51, LIBCMT 20, sonstige benannte CRT 11, XAPILIB 2 (`SwitchToFiber` push/pop). **D3D, DSOUND, XMV und XPP enthalten keinen einzigen fs:-Zugriff auf Offset 0.** Alle `fs:[0]`-Stellen sind absolute disp32-Zugriffe ohne Basisregister.

Eingetragene Handler (`cpu_sehhandlers.py`, korrigiert durch `verify_cpu_seh.py`):

- Im `.text` stehen 1117 C++-EH-Thunks `mov eax, FuncInfo; jmp ___CxxFrameHandler (0x166977)`. Von den 1495 Registrierungen `mov eax, fs:[0]` tragen mindestens 1477 einen dieser Thunks ein (1107 verschiedene; mit einem ±12-Byte-Fenster um den Load zählt `verify_cpu_seh2.py` 1488 Stellen und 1114 verschiedene Thunks, 3 weitere haben das `push handler` erst später). Die ursprüngliche Zahl 1238 war falsch, weil `cpu_sehhandlers.py` nur ein `push imm` **vor** dem Load sucht; MSVC erzeugt auch `mov eax,fs:[0]; push -1; push handler`.
- 42 Aufrufe gehen an `__SEH_prolog` (0x167dc8), der den Handler 0x168dcc einträgt (MSVC-`_except_handler3`, Vermutung nach Muster). `FUN_00150d40` (cpuid) trägt 0x168dcc inline ein.

### 2.2 Wie TLS funktioniert (belegt)

**TLS-Verzeichnis 0x2a5d2c:** StartRaw = 0, EndRaw = 0, AddressOfIndex = **0x2f3d18** (bss), Callbacks = 0, SizeOfZeroFill = **0x20**.

**Entry 0xf84dd:**

```
0xf84f5  eax = SizeOfZeroFill - StartRaw ; + EndRaw ; +0xF ; &~0xF ; +4   -> 0x24
0xf8515  mov [0x310394], eax             ; TlsDataSize = 0x24
0xf851a  cdq / idiv ecx(=-4)             ; eax = -9
0xf852f  mov [ecx = [0x2a5d34] = 0x2f3d18], eax   ; _tls_index = -9
```

**`XAPILIB::CreateThread` (0xf9a72)** ruft `PsCreateSystemThreadEx` (ord 255, Slot 0x293694) mit 10 Argumenten auf, darunter `[0x310394]` als 4. Argument (TlsDataSize) und als SystemRoutine `0xf99da` (`XapiThreadStartup`, in Ghidra keine eigene Funktion).

**`XapiThreadStartup` 0xf99da:**

```
0xf99ea  mov eax, fs:[0x28]          ; KTHREAD
0xf99f3  mov edx, [eax+0x28]         ; TlsData
0xf99f6  add edx, 4 ; mov [edx-4], edx   ; TlsData[0] = &TlsData[1]  (= TLS-Block)
         rep movs  (EndRaw-StartRaw = 0 Byte)  ; rep stos (0x20 Byte Null)
0xf9a41  call 0xf98b5 (Arg 1)        ; Thread-Notify-Routinen „Start“ (Nachtrag der Verifikation)
0xf9a49  call [ebp+8]                ; StartRoutine(StartContext)
0xf9a51  call 0xf98b5 (Arg 0)        ; Thread-Notify-Routinen „Ende“, u. a. CRT-Routine 0x16ecf4 → _freeptd
0xf9a6b  call [0x293690]             ; ord 258 = PsTerminateSystemThread laut kernel_imports.tsv (1 Push-Argument, danach int3)
```

Der ganze Ablauf steht in einem `__try` (`__SEH_prolog` bei 0xf99e1, Filter ruft `UnhandledExceptionFilter` 0xf999e).

**Zugriffsmuster** (z. B. `GetLastError` 0xf6e54):

```
mov eax,[0x2f3d18]        ; -9
mov ecx, fs:[4]           ; StackBase
mov eax,[ecx+eax*4]       ; [StackBase-0x24] = TlsData[0] = TLS-Block
mov eax,[eax+4]           ; LastError
```

Auf der Xbox liegt die TLS-Area also in den obersten 0x24 Byte des Thread-Stacks (`TlsData = StackBase - 0x24`; abgeleitet daraus, dass `[StackBase-0x24]` den von `XapiThreadStartup` in `[TlsData]` geschriebenen Zeiger liefern muss).

**Threads ohne TLS (KTHREAD.TlsData = 0):** `_getptd` (0x1692ec–0x169309) und `_freeptd` (0x169378–0x169395) nehmen dann statt `TLS+8` den **globalen** Slot 0x310734. Alle solchen Threads teilen sich also ein `_tiddata` (errno, strtok-Zustand usw.). `GetLastError`/`SetLastError` prüfen TlsData **nicht** und dereferenzieren den TLS-Anker immer. Für den Port heißt das: Nur der Entry-Startthread darf TlsData = 0 haben, jeder andere adoptierte Thread braucht echte TlsData.

*Nachtrag der Verifikation:* Auch der Entry-Startthread kann `SetLastError` erreichen. Nach `CreateThread` ruft der Entry `CloseHandle` (0xf8547 → 0xf7177); schlägt `NtClose` fehl, folgt `XapiSetLastNTError` → `SetLastError` (0xf718b → 0xf6eaa → 0xf6e7c), und das ohne TlsData-Prüfung. Die HLE sollte deshalb auch für den Entry-Thread den TLS-Anker auf einen gültigen Dummy-Block zeigen lassen und nur `KTHREAD.TlsData` auf 0 setzen. Weitere XBE-Threads gibt es nicht: `CreateThread` hat genau einen Aufrufer (0xf8531, Main-Thread 0xf8469), und `PsCreateSystemThreadEx` wird nur aus `CreateThread` gerufen (Slot 0x293694, 1 Aufrufstelle).

**Belegte Felder des XBE-TLS-Blocks (0x20 Byte):**

| Offset | Bedeutung | Beleg |
|---|---|---|
| +0x04 | LastError | `GetLastError` 0xf6e75, `SetLastError` 0xf6ea1 |
| +0x08 | LIBCMT `_tiddata*` | `_getptd` 0x169301 (`add edi,8`), `_mtinit` 0x169499 |
| +0x0C | aktuelle Fiber | `SwitchToFiber` 0x17096e/0x17097a, Spiel `FUN_0013f250` 0x13f44d, `FUN_0013f590` 0x13f5df |
| +0x10..+0x1F | Fiber-Struktur des per `ConvertThreadToFiber` umgewandelten Threads: {Param, StackBase (aus KTHREAD+0x1C), StackLimit = **0**, gesicherter esp}. Das vierte Dword (+0x1C) schreibt `SwitchToFiber` (0x170974 `mov [ecx+0xc],esp`). | 0x1709a1–0x1709b9, 0x170974 |

**Belegte KTHREAD-Felder (über `fs:[0x28]`):**

| Offset | Bedeutung | Beleg |
|---|---|---|
| +0x1C | StackBase | 0x1709af |
| +0x28 | TlsData | 0xf99f3, 0xf9ac9, 0x1692ec, 0x169378, 0xf84a5 |
| +0x12C | Thread-ID | 0xf9ae6 |

Die Namen sind eine Vermutung nach xboxkrnl-Headern, die Verwendung ist belegt.

**KPRCB (über `fs:[0x20]`):** Gelesen wird nur `+0x250`. Wenn ungleich 0, ruft der Code `[p+0x14](code,arg)` auf: Codes 0x0a/0x0b in der XAPI-Heap-Allokation/-Freigabe (`FUN_000f52cc` 0x0a, `f5a83` 0x0b, `f5c77` erst 0x0b bei 0xf6366, dann 0x0a bei 0xf639b), 2 vor `HalReturnToFirmware` (0xf82c9), 0x0c im UnhandledExceptionFilter (0xf99b0). Der Main-Thread liest `[p+0x24]` und schreibt dorthin ein Flag und den TLS-Offset (0xf8483–0xf84b1). D3D liest bei 0x23a1f5 `q = [p+0x20]` und schreibt, falls q ≠ 0, Device-Adressen (u. a. `device+0x1984`) in die Struktur bei q. Das ist der Debug-Monitor-/PIX-Hook (**Vermutung**: xbdm). **Auf Retail und im Port muss der Wert 0 sein.**

### 2.3 Alle 38 Stellen mit Offset ungleich 0

`disp@` = Byte-Offset des disp32 innerhalb des Befehls (für den Patch).

| Adresse | Bytes | Befehl | Funktion | Lib | disp@ | Zweck |
|---|---|---|---|---|---|---|
| `000f5a41` | `64a120000000` | `mov eax, fs:[0x20]` | FUN_000f52cc (XAPI-Heap-Alloc, Vermutung) | XAPILIB | +2 | Debug-Hook 0x0a |
| `000f5ab8` | `64a120000000` | `mov eax, fs:[0x20]` | FUN_000f5a83 (XAPI-Heap-Free, Vermutung) | XAPILIB | +2 | Debug-Hook 0x0b |
| `000f634f` | `64a120000000` | `mov eax, fs:[0x20]` | FUN_000f5c77 (XAPI-Heap-ReAlloc, Vermutung) | XAPILIB | +2 | Debug-Hook 0x0b |
| `000f6384` | `64a120000000` | `mov eax, fs:[0x20]` | ?(after FUN_000f5c77) | XAPILIB | +2 | Debug-Hook 0x0a |
| `000f6e54` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | XAPILIB::GetLastError | XAPILIB | +4 | IRQL-Check |
| `000f6e60` | `64a128000000` | `mov eax, fs:[0x28]` | XAPILIB::GetLastError | XAPILIB | +2 | toter Load (wird sofort überschrieben) |
| `000f6e6b` | `648b0d04000000` | `mov ecx, fs:[4]` | XAPILIB::GetLastError | XAPILIB | +3 | TLS |
| `000f6e7c` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | XAPILIB::SetLastError | XAPILIB | +4 | IRQL-Check |
| `000f6e88` | `64a128000000` | `mov eax, fs:[0x28]` | XAPILIB::SetLastError | XAPILIB | +2 | toter Load |
| `000f6e8e` | `648b0d04000000` | `mov ecx, fs:[4]` | XAPILIB::SetLastError | XAPILIB | +3 | TLS |
| `000f82b9` | `64a120000000` | `mov eax, fs:[0x20]` | FUN_000f829c (Reboot über HalReturnToFirmware) | XAPILIB | +2 | Debug-Hook 2 |
| `000f8473` | `64a120000000` | `mov eax, fs:[0x20]` | Main-Thread 0xf8469 (gap) | XAPILIB | +2 | Debug-Hook: TLS-Info |
| `000f848f` | `64a128000000` | `mov eax, fs:[0x28]` | Main-Thread 0xf8469 (gap) | XAPILIB | +2 | KTHREAD.TlsData |
| `000f8495` | `648b3d04000000` | `mov edi, fs:[4]` | Main-Thread 0xf8469 (gap) | XAPILIB | +3 | TLS |
| `000f999e` | `64a120000000` | `mov eax, fs:[0x20]` | XAPILIB::UnhandledExceptionFilter | XAPILIB | +2 | Debug-Hook 0x0c |
| `000f99ea` | `64a128000000` | `mov eax, fs:[0x28]` | XapiThreadStartup 0xf99da | XAPILIB | +2 | TLS-Init |
| `000f9ab7` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | XAPILIB::CreateThread | XAPILIB | +4 | IRQL-Check (Fehlerpfad) |
| `000f9ac3` | `64a128000000` | `mov eax, fs:[0x28]` | XAPILIB::CreateThread | XAPILIB | +2 | TlsData≠0? (Fehlerpfad) |
| `000f9ae0` | `64a128000000` | `mov eax, fs:[0x28]` | XAPILIB::GetCurrentThreadId | XAPILIB | +2 | KTHREAD+0x12C |
| `0013f443` | `648b0d04000000` | `mov ecx, fs:[4]` | FUN_0013f250 (Spiel, Konstruktor eines Datei-/Stream-Objekts mit Lade-Fiber) | Spiel | +3 | GetCurrentFiber inline |
| `0013f5d5` | `648b3d04000000` | `mov edi, fs:[4]` | FUN_0013f590 (Spiel) | Spiel | +3 | GetCurrentFiber inline |
| `001692d0` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | FUN_001692d0 (`_getptd`, Vermutung) | LIBCMT | +4 | IRQL≥2 → KeBugCheck(0xa) |
| `001692e6` | `64a128000000` | `mov eax, fs:[0x28]` | FUN_001692d0 | LIBCMT | +2 | TlsData≠0? |
| `001692f7` | `648b0d04000000` | `mov ecx, fs:[4]` | FUN_001692d0 | LIBCMT | +3 | TLS+8 |
| `0016935e` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | 0x169352 (`_freeptd`, Vermutung: gibt `[ptd+0x24/0x2c/0x34/0x3c/0x44/0x48/0x54]` frei, nullt den Slot; 1 Aufrufer 0x16ed15) | LIBCMT | +4 | IRQL≥2 → KeBugCheck(0xa) |
| `00169372` | `64a128000000` | `mov eax, fs:[0x28]` | 0x169352 | LIBCMT | +2 | TlsData≠0? |
| `00169383` | `648b0d04000000` | `mov ecx, fs:[4]` | 0x169352 | LIBCMT | +3 | TLS+8 |
| `0016948f` | `648b0d04000000` | `mov ecx, fs:[4]` | FUN_00169467 (`_mtinit`, Vermutung) | LIBCMT | +3 | TLS+8 setzen |
| `00170880` | `648b0d04000000` | `mov ecx, fs:[4]` | Fiber-Startroutine 0x17086b | XAPILIB | +3 | TLS+0xC |
| `00170955` | `648b0d04000000` | `mov ecx, fs:[4]` | XAPILIB::SwitchToFiber | XAPILIB | +3 | TLS+0xC |
| `00170993` | `648b0d04000000` | `mov ecx, fs:[4]` | XAPILIB::ConvertThreadToFiber | XAPILIB | +3 | TLS+0x10 |
| `001709a9` | `64a128000000` | `mov eax, fs:[0x28]` | XAPILIB::ConvertThreadToFiber | XAPILIB | +2 | KTHREAD+0x1C |
| `0023a1e5` | `64a120000000` | `mov eax, fs:[0x20]` | FUN_00239d20 (D3D Device-Init, aus Direct3D_CreateDevice) | D3D | +2 | PIX-Hook |
| `00246d37` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | FUN_00246d30 (DSOUND Auto-IRQL-Guard) | DSOUND | +4 | Raise nur, wenn IRQL<2 |
| `00246d7a` | `64a158000000` | `mov eax, fs:[0x58]` | FUN_00246d7a | DSOUND | +2 | in DPC → KeSaveFloatingPointState |
| `00246d9f` | `64a158000000` | `mov eax, fs:[0x58]` | FUN_00246d9f | DSOUND | +2 | in DPC → KeRestoreFloatingPointState |
| `00246e0d` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | DSOUND::DirectSoundEnterCriticalSection | DSOUND | +4 | IRQL=0 → RtlEnterCriticalSection(0x263b20) |
| `0024ba50` | `640fb60524000000` | `movzx eax, byte fs:[0x24]` | DSOUND::CMcpxStream_Flush | DSOUND | +4 | IRQL merken |

Alle 38 haben einen absoluten disp32 ohne Basis- oder Indexregister. Die Befehlslängen sind 6, 7 und 8 Byte.

### 2.4 Strategien für die Nicht-Null-Offsets

**A – disp32 auf TEB.TlsSlots umschreiben (empfohlen).**

1. Der Host reserviert beim Start vor allen anderen DLL-Initialisierungen 5 Slots mit `TlsAlloc`: S_TLS, S_KTHREAD, S_PRCB, S_IRQL, S_DPC. Er prüft, dass jeder Index kleiner als 64 ist, denn nur `TlsSlots[0..63]` liegen inline im TEB bei `fs:[0xE10+4k]`. Danach setzt er per `TlsSetValue` einen Testwert und liest ihn mit `__readfsdword(0xE10+4k)` zurück.
2. An jeder der 38 Stellen wird das disp32 an der Position aus der Tabelle (+2/+3/+4) ersetzt:
   - `0x04` → `0xE10+4*S_TLS`
   - `0x20` → `0xE10+4*S_PRCB`
   - `0x24` → `0xE10+4*S_IRQL` (Byte-Zugriff liest das Low-Byte)
   - `0x28` → `0xE10+4*S_KTHREAD`
   - `0x58` → `0xE10+4*S_DPC`

   Beispiel: `64 A1 28 00 00 00` wird mit Slot 7 zu `64 A1 2C 0E 00 00`. Vor dem Patch werden die Originalbytes aus `fs_sites.tsv` verglichen; bei Abweichung bricht der Host ab.
3. Werte pro Thread, die die HLE in jedem Thread setzt, der Xbox-Code ausführt (vom HLE-`PsCreateSystemThreadEx` erzeugte Threads, Startthread für den Entry, HLE-DPC-/Timer-Thread):
   - **S_TLS**: `TlsData + TlsDataSize`. Dann gilt `[S_TLS + (-9)*4] = [TlsData] = TLS-Block`. TlsData wird pro Thread mit TlsDataSize Byte (Argument 4 von `PsCreateSystemThreadEx`, hier 0x24) alloziert.
   - **S_KTHREAD**: Fake-KTHREAD, mindestens 0x130 Byte, mit +0x1C StackBase, +0x20 StackLimit, +0x28 TlsData, +0x12C Thread-ID. Den Entry-Startthread mit TlsData = 0 behandelt der XAPI-Code selbst, z. B. `cmp [eax+0x28],0` bei 0xf9ac9. S_TLS muss trotzdem auf einen gültigen Dummy-Block zeigen (siehe 2.2, `CloseHandle`-Fehlerpfad).
   - **S_PRCB**: Zeiger auf einen genullten Fake-KPRCB von mindestens 0x254 Byte. Feld +0x250 = 0, dann sind alle Debug-Hooks aus.
   - **S_IRQL**: aktueller IRQL. Normal 0; die HLE von `KfRaiseIrql`/`KeRaiseIrqlToDpcLevel`/`KfLowerIrql` aktualisiert den Slot. Im HLE-DPC-Thread steht 2.
   - **S_DPC**: 0, bzw. 1, solange die HLE eine DPC-Routine ausführt. Dann ruft DSOUND die `KeSave/RestoreFloatingPointState`-HLE auf, die ein No-op sein darf.
4. Vorteile: Die Befehlslänge ändert sich nicht, es gibt keine Trampoline und keine Flag- oder Register-Seiteneffekte, die Semantik pro Thread bleibt erhalten, und es hängt nicht vom Windows-Stack ab. Das ist wichtig, weil `fs:[4]` damit vom echten `TEB.StackBase` entkoppelt wird; siehe Fibers in 2.5.
5. Risiken:
   - (a) TlsAlloc-Index ≥ 64 → im Host früh allozieren und hart prüfen.
   - (b) Host-Threads, die Xbox-Code ohne „Adoption“ betreten, lesen Slot 0 und stürzen ab. Jeder Einstieg (Callbacks, DPC, Timer, Audio) braucht einen Adopt-Wrapper.
   - (c) Eine übersehene Stelle crasht. Recursive Descent plus Lückensweep haben alle bekannten gefunden, dazu 9, die ein Ghidra-only-Scan übersieht. Restrisiko: Code, der nur über unerkannte Zeiger erreichbar ist **und** in Datenlücken liegt. Die `gap-data`-Treffer haben keine sinnvollen Offsets (alle mit Basisregister oder unsinnig).
   - (d) Die Patches verändern .text. Funktionsvergleiche des Decomp-Projekts müssen die 38 Stellen kennen.

**B – `call`-Patch:** An jeder Stelle steht ein 5-Byte-`call helper`, mit 1–3 NOPs aufgefüllt. Platz ist überall genug (6/7/8 Byte).
- Der Helper müsste je nach Zielregister (eax/ecx/edi) und Offset eine eigene Variante sein, alle Register außer dem Ziel erhalten und braucht Stack.
- Es gibt keinen Vorteil gegenüber A, nur mehr Aufwand und Fehlerquellen. Nur sinnvoll, wenn Werte berechnet statt gelesen werden müssten.

**C – Funktionen ersetzen** statt Bytes zu patchen: `GetLastError`, `SetLastError`, `GetCurrentThreadId`, `CreateThread`, `XapiThreadStartup`, die Fiber-Funktionen, `_getptd` ×2, `_mtinit`, die XAPI-Heap-Hooks, DSOUND/D3D.
- Langfristig ohnehin der Weg: Mit jeder ersetzten Funktion entfällt der Patch.
- Allein reicht es nicht: Die 2 Inline-Stellen im Spielcode (`FUN_0013f250`, `FUN_0013f590`) und die Main-Thread-Routine bleiben übrig, solange sie nicht dekompiliert sind. A und C kombinieren.

**D – eigenes FS-Segment mit Fake-KPCR (LDT):** Unter 64-Bit-Windows (WoW64) sind LDT-Einträge nicht verfügbar (**Vermutung**; bekanntes Verhalten von `NtSetInformationProcess(ProcessLdtInformation)` auf x64). Die FS-Basis lässt sich im Compatibility-Mode nicht setzen. **Verworfen.**

**E – TEB-Felder 0x20/0x24/0x28/0x58 mit Xbox-Werten überschreiben:** Bricht Windows. `GetCurrentProcessId`/`GetCurrentThreadId` lesen ClientId, RPC/COM benutzt ActiveRpcHandle, 0x58 liegt in User32Reserved. Die HLE-Hostfunktionen laufen auf demselben Thread. **Verworfen.**

**F – `fs:[4]` unverändert lassen** und jeden Xbox-Thread auf einem selbst allozierten Stack laufen lassen, dessen oberste 0x24 Byte die TLS-Area sind, mit passend gesetztem `TEB.StackBase`. Das funktioniert für Threads, kollidiert aber mit Fibers:
- XAPI-Fibers ändern StackBase nicht, Windows braucht aber den korrekten StackBase für die Exception-Dispatch-Prüfung.
- Windows-Fibers ändern StackBase und zerstören damit den TLS-Anker.
- **Verworfen zugunsten von A.**

### 2.5 `fs:[0]` (SEH) – bleibt, aber mit Bedingungen

Semantik und Struktur (`EXCEPTION_REGISTRATION_RECORD`) sind auf Xbox und Windows identisch. Damit Windows-Exceptions durch die Kette laufen, gilt:

1. **Kernel-HLE:** `RtlRaiseException` (ord 302, Slot 0x2935a0; 3 Aufrufstellen: `XAPILIB::RaiseException` 0xf9938, das von FUN_0016aff0 (C++-`throw`, Vermutung) gerufen wird, sowie die XAPI-Heap-Funktionen FUN_000f52cc 0xf5a10 und FUN_000f5c77 0xf6303) und `RtlUnwind` (ord 312, Stub 0x170aae) müssen direkt auf die ntdll-Gegenstücke abgebildet werden. `UnhandledExceptionFilter`/`SetUnhandledExceptionFilter` sind XAPI-intern.
2. **Stack-Grenzen:** Windows dispatcht nur Records zwischen `TEB.StackLimit` und `TEB.StackBase`.
   - **Das Spiel nutzt XAPI-Fibers als Coroutinen für das Laden von Dateien**: `FUN_0013f250` ist der einzige Aufrufer von `CreateFiber` und `ConvertThreadToFiber`. Laut Decompilat ist es der Konstruktor eines Datei-/Stream-Objekts (korrigiert, vorher „Task-Konstruktor“): Er baut aus `param_2` einen Pfad (Präfix 0x2d7ad8, Suffix 0x30e598, `/` → `\`), sucht ihn per FUN_0013fe10 oder öffnet ihn mit FUN_000f76e0 (`0x80000000` = GENERIC_READ, OPEN_EXISTING 3, Flags 0x68000000). Nur wenn `param_3 ≠ 0` ist, legt er **eine** Fiber an: `CreateFiber(0x4000, FUN_0013f1a0, this)`, also 16 KB Stack. `ConvertThreadToFiber(0)` läuft prozessweit nur einmal (globales Flag `0x30e594`); danach liest der Code die aktuelle Fiber inline (0x13f443). `FUN_0013f250` hat 35 direkte Aufrufstellen in 28 Funktionen (Byte-Scan `verify_cpu_callsites.py`/`verify_cpu_fiber.py`; `callgraph.tsv` nennt 27 Aufrufer, die Stelle 0xccc78 liegt außerhalb einer Ghidra-Funktion). `FUN_0013f590` (2 Stellen) und `FUN_0013f1a0` (1 Stelle) rufen `SwitchToFiber`, `FUN_0013f590` auch `DeleteFiber`.
   - Fiber-Stacks kommen aus dem Kernel: `CreateFiber` ruft `MmCreateKernelStack` (ord 169, Slot 0x29370c, 0x1708da; Größe mindestens 0x3000, auf Seiten gerundet, Default aus 0x10130), `DeleteFiber` ruft `MmDeleteKernelStack(StackBase, StackLimit)` (ord 170, 0x170946). Die HLE muss beide liefern, solange die Fiber-Funktionen nicht ersetzt sind.
   - XAPI-`SwitchToFiber` (0x17094f) tauscht nur esp und `fs:[0]`, nicht StackBase/StackLimit. Jede Exception auf einem Fiber-Stack würde von Windows verworfen. Beispiele: `IsBadReadPtr`/`IsBadWritePtr` (XAPI, nutzen SEH), C++-`throw`, das `__try` um `cpuid` in `FUN_00150d40`.
   - **Abhilfe:** `CreateFiber`/`DeleteFiber`/`SwitchToFiber`/`ConvertThreadToFiber` und die Startroutine 0x17086b ersetzen. Der Ersatz pflegt beim Umschalten zusätzlich `TEB.NtTib.StackBase/StackLimit` (und `TEB.DeallocationStack` 0xE0C) aus der XAPI-Fiber-Struktur: `[f+4]` StackBase, `[f+8]` StackLimit, laut `CreateFiber` 0x1708f5–0x1708fd.
   - Achtung: Die von XAPI-`ConvertThreadToFiber` angelegte Thread-Fiber hat `[f+8] = 0` (0x1709b2 `and [ecx+8],0`) und `[f+4]` aus dem Fake-KTHREAD. Der Ersatz muss für diese Fiber die echten TEB-Werte des Threads beim Konvertieren sichern, statt die Strukturwerte zu kopieren.
   - Mit A ist `fs:[4]` nicht mehr der TLS-Anker, das Ändern von StackBase ist also gefahrlos.
3. **SEHOP** (unter Windows 10/11 laut Exploit-Protection-Default aktiv, **Vermutung**):
   - Die Kette muss mit dem ntdll-Terminal-Record enden.
   - XAPI-`CreateFiber` legt für neue Fibers `ExceptionList = 0xFFFFFFFF` an (0x170931 `or dword ptr [ecx],-1`).
   - Abhilfe: Der Ersatz-`CreateFiber` kopiert den Terminal-Record des Threads, also das letzte Element der Kette, auf den neuen Fiber-Stack. Alternativ SEHOP für die Host-EXE abschalten.
4. **Handler-Validierung (SafeSEH):** Die Handler (1117 `___CxxFrameHandler`-Thunks, korrigiert von 1238; `_except_handler3` 0x168dcc) liegen im XBE-Bereich.
   - Werden die XBE-Sektionen als Sektionen der Host-EXE gelinkt (Image), muss die EXE mit **`/SAFESEH:NO`** gebaut sein.
   - Werden sie als privater Speicher gemappt, müssen die Seiten ausführbar sein (`PAGE_EXECUTE_*`). Bei aktivem DEP lehnt der Dispatcher Handler auf nicht ausführbaren Nicht-Image-Seiten ab (**Vermutung** zum Verhalten von `RtlIsValidHandler`).
   - Früher Selbsttest: `XAPILIB::IsBadReadPtr(0,4)` (0x1709c9) muss TRUE liefern.

---

## 3. Privilegierte, I/O- und Sonderbefehle (nur echter Code)

| Adresse | Bytes | Befehl | Funktion | Lib | Bewertung |
|---|---|---|---|---|---|
| `000f3e0b` | `cd 2d` | `int 0x2d` (eax=1, ecx=&ANSI_STRING) | XAPILIB::OutputDebugStringA | XAPILIB | Debug-Service-Interrupt. Ohne Debugger unter Windows eine Breakpoint-Exception (Vermutung), gefolgt von `int3` 0xf3e0d. **Funktion ersetzen** (Aufrufer: FUN_000edc70, FUN_0016eb2b). |
| `000f641f` | `0f 31` | `rdtsc` | XAPILIB::QueryPerformanceCounter | XAPILIB | Erlaubt, aber **`QueryPerformanceFrequency` (0xf642c) liefert fest 0x2BB5C755 = 733 333 333**. Auf dem PC läuft die TSC anders, das Spieltiming wäre falsch. **QPC und QPF ersetzen.** Aufrufer QPC (0xf641b): Spiel FUN_000d2b90 (0xd2bcd), D3D-Code ohne Ghidra-Funktion hinter FUN_00241810 (0x241a12/0x241a61/0x241b45), XMV FUN_00264775/FUN_00264962/FUN_00264fdf. Aufrufer QPF: Spiel FUN_000d2b90 (0xd2bb1), D3D 0x241b4e und FUN_00241c40, XMV FUN_00264360. Das Spiel berechnet `1.0f/Frequenz` einmalig zur Laufzeit (0xd2bb6–0xd2bc2), also genügt ein konsistentes Paar QPC/QPF. Die Konstante kommt sonst nirgends vor (Byte-Suche nach int/float/double, auch Kehrwerte und MHz/kHz-Varianten, `verify_cpu_hw.py`). |
| `000f98a3` / `000f98aa` | `fa` / `fb` | `cli` / `sti` | FUN_000f9804 (vom Main-Thread 0xf8469 als erstes aufgerufen) | XAPILIB | **Patcht Kernel-Code**: Für Kernel-Build 3944/4034/4039/4817/4818 (`XboxKrnlVersion`+4, ord 324) wird an einer Kernel-Adresse `0x20c5048d` durch `0x28c5048d` ersetzt (`MmQueryAddressProtect`/`MmSetAddressProtect`, ord 179/182). Die Adresse hängt zusätzlich von Bit 1 von `[XboxHardwareInfo]` (ord 322) ab: Bit 1 = 0 → 3944: 0x8002ABBA, 4034: 0x8002AB88, 4039: 0x8002AB98, 4817/4818: 0x800299A0; Bit 1 = 1 → 3944: 0x80039749, 4034: kein Patch, 4039: 0x800398CD, 4817/4818: 0x8003838F (0xf9844–0xf9878). Gepatcht wird nur, wenn dort tatsächlich `0x20c5048d` steht (0xf9883). **HLE muss einen anderen Build melden** (z. B. 5838); dann kehrt die Funktion vor `cli` zurück (0xf983a/0xf9842). Die Thunk-Datenzeiger für ord 322/324 werden aber immer gelesen (0xf9804–0xf9811). |
| `00150d79`, `00150dc2`, `00150e17` | `0f a2` | `cpuid` | FUN_00150d40 (aus Init-Stub-Thunk 0x230dc0 über FUN_00151150) | Spiel | Erlaubt. CPU-Typ 0–6 nach `[0x30ef44]`. Bug: Die zweite Abfrage setzt `edx` statt `eax` auf 0x80000001. **Der Wert hat keine Folgen**, denn FUN_00151150 setzt immer feste Funktionszeiger 0x150ef0/0x151030 (0x151164–0x151176). |
| `0023a0e0` | `0f 09` | `wbinvd` | FUN_00239d20 (Device-Init) | D3D | **#GP im User-Mode**. D3D-Init muss ersetzt werden. |
| `0023b780` | `0f 31` | `rdtsc` | FUN_0023b780 (aufgerufen von FUN_0023b980 und FUN_0023bd00) | D3D | erlaubt, D3D-intern (Zeitstempel-Differenz in `[esi+0x1d4]`) |
| `0023b80c` | `ec` | `in al,dx` (dx=0x80C0) | FUN_0023b790 = **GPU-ISR** (`KeInitializeInterrupt(…,FUN_0023b790,…)`, HalGetInterruptVector(3)) | D3D | **#GP**. Port im MCPX-ACPI-I/O-Bereich, Bit 5 = Halbbild-Status (Vermutung). |
| `0023b9fd` | `ec` | `in al,dx` (dx=0x80C0) | FUN_0023b980 | D3D | **#GP** |
| `0023c9b9` | `ee` | `out dx,al` (dx=0x80C0) | D3D8::CMiniport_InitHardware | D3D | **#GP**. Daneben `HalReadWritePCISpace` (ord 46) für PCI-Config 0x4C. |
| `002921c5` / `002921d6` | `fa` / `fb` | `cli` / `sti` | FUN_002921b9 (aus DPC FUN_002927d3) | XPP | **#GP**. Liest unter cli das APU-Zählerregister 0xFE80200C und `[[x+8]+0x80]` (Vermutung: OHCI-HCCA-Framenummer). USB-Stack, wird ersetzt. |
| 17× | `cc` | `int3` | nach noreturn-Aufrufen/Padding (z. B. 0x1708af nach `KeBugCheck(0)`, 0xf9a71, 0xf902e nach `HalReturnToFirmware`) | div. | 13 davon werden nie ausgeführt. **Korrigiert:** Nicht „alle nie ausgeführt“. 0xf3e0d steht direkt hinter `int 0x2d` (Debug-Service-Konvention; ob sie ausgeführt wird, hängt vom Handler ab, Vermutung). Die drei D3D-Stellen 0x23bcf3, 0x23bf4b und 0x23c0f8 sind absichtliche Breakpoints in Fehlerpfaden. Sie stehen nach einer **zurückkehrenden** Debug-Ausgabe (`call 0x23e600` mit „GPU generated an error!…“ 0x243208 bzw. Format 0x2431c0, `add esp,0x20`, `int3`) und würden unter Windows ohne Debugger als unbehandelte Breakpoint-Exception abbrechen. `OutputDebugStringA` und D3D werden ohnehin ersetzt. |
| D3D 15×`movntq`, 4×`prefetchnta`, 3×`sfence`, 1×`emms`; XMV 43×`emms` | | MMX/SSE1 | D3D/XMV | | auf jeder SSE-CPU in Ordnung |

**Nicht gefunden (in echtem Code):** `lidt`/`lgdt`/`lldt`/`ltr`, `mov crN`/`drN`, `rdmsr`/`wrmsr`, `invd`, `invlpg`, `sysenter`/`syscall`, `iret`, `hlt`, `int n` (außer 0x2d), `ins`/`outs`, `pushfd`/`popfd`, Laden von Segmentregistern, `lds`/`les`/`lss`. Alle solchen Treffer eines naiven Sweeps liegen in Datenbereichen (`--all`).

**Uniprozessor-Annahme:** Im gesamten erreichten Code gibt es **kein einziges `lock`-Präfix** und keine `Interlocked*`-Importe. Es gibt genau ein `cmpxchg` ohne `lock` (DSOUND 0x24a827) und ein `xchg [esp],eax` (CRT).
- Gegenseitiger Ausschluss läuft über das Anheben des IRQL (`KeRaiseIrqlToDpcLevel`, 23 Stellen im Decompilat) und `RtlEnterCriticalSection`.
- Konsequenz für die HLE: Das Anheben des IRQL braucht ein globales rekursives Lock, unter dem auch die HLE-DPCs und -Timer laufen.
- Für den Anfang die Prozessaffinität auf einen Kern setzen (**Vermutung**: reduziert Races in UP-optimiertem Code, ersetzt aber das Lock nicht, weil Windows auch auf einem Kern präemptiert).

---

## 4. Hardware-/MMIO-Adressen

Quelle: `cpu_hw_refs.tsv`, 332 Zeilen (330 + 2 von der Verifikation ergänzte APU-Immediates, `scripts\verify_cpu_fixtsv.py`), Klassen in Spalte `rel`. Reine Konstanten (HRESULTs 0x8007000E/0x80004005, NTSTATUS 0xC0000xxx, Floats 0xBF800000, Füllmuster 0xFEEEFEEE/0xDEADBEEF/0xF00DBEEF) sind herausgefiltert.

| Bereich | Wer (Funktionen) | Anzahl | Art |
|---|---|---|---|
| **APU 0xFE800000–0xFE85FFFC** | DSOUND: 43 Funktionen (42 mit Speicheroperand), u. a. `CMcpxAPU_SetMixBinHeadroom`, `CMcpxAPU_SynchPlayback`, `CMcpxVoiceClient_SetFilter`/`SetMixBins`/`SetPitch`/`SetVolume`, FUN_0024cdea, FUN_0024d963, FUN_0024f87d | 266 Speicheroperanden + 4 Immediates (korrigiert, vorher 2) | 257 absolute disp32, 9 basisrelativ `[reg+0xFE8xxxxx]`. Immediates: `mov [ebp-4],0xfe820400` / `cmp [ebp-4],0xfe82043c` (Zeigerschleife über APU-Register, FUN_0024caca), `mov edi,0xfe804028` (FUN_0024f9a6), `add [edi+0x10],0xfe836000` (FUN_0024a0f2) |
| APU 0xFE80200C | XPP FUN_002921b9 (0x2921d0, unter cli) | 1 | Lesen |
| **AC97 0xFEC00000/0xFEC00100** | DSOUND FUN_00250384 … FUN_00250ae4 (11 Funktionen): Codec-Register `[reg*2+0xFEC00000]` (0x250427), Busmaster 0xFEC0012C/130/134/17C und `[x+0xFEC00100..]` | 22 | Speicherzugriffe |
| **NV2A 0xFD000000** | D3D: FUN_0023c39a speichert die Basis `mov [ecx],0xfd000000`; dort auch 0xFD001804, 0xFD600140, 0xFD009140. FUN_0023bcb0/0023bd00/0023c040 lesen 0xFD40014C | 1 Immediate + 7 Zugriffe | Die meisten GPU-Registerzugriffe laufen über den gespeicherten Basiszeiger und sind daher nicht als Immediate sichtbar. |
| **Physisches Fenster 0x80000000** | D3D FUN_00239d20: `mov [0x80000000],edx` (0x23a0da, gefolgt von `wbinvd`) und `mov [0x80000000],0xdeadbeef` (0x23a122). Schreibt physische Adresse 0 (Pushbuffer-Sprung, Vermutung) | 2 | Schreiben |
| `or reg,0x80000000` (phys→virt) | D3D: D3D_DestroyResource, D3DVertexBuffer_Lock2, D3DPalette_Lock2, D3D_MakeRequestedSpace_8, D3D_BlockOnTime, FUN_00238c10, FUN_0023ab60, FUN_0023bd00, FUN_0023c9ef | 12 | Adressumrechnung |
| `or ecx,0xF0000000` (WC/Tile-Alias) | D3D8::Lock2DSurface 0x23ade8 | 1 | Adressumrechnung |
| `and reg,0x0FFFFFFF` (virt→phys, Nachtrag der Verifikation) | D3D 18× (u. a. `CDevice_KickOff`, `D3D_SetTileNoWait`, `D3DDevice_CreateTexture2`, FUN_00239d20), DSOUND 3× (FUN_0024c15a), XPP 2× (FUN_0028eb75/FUN_0028ed5b, als `mov`-Maske) | 23 | Adressumrechnung, nicht in `cpu_hw_refs.tsv`; im Spielcode kommt die Maske nicht vor |
| `and x,0xF0000000; cmp …,0x80000000` | D3D FUN_0023c521, XPP FUN_0028eb75/FUN_0028ed5b | 3 | Adressraumprüfung (in den Konstanten der Rohdaten) |
| **USB OHCI 0xFED00000** | XPP FUN_0028c8f0: Ressourcendeskriptor (Länge 0x1000) + `HalGetInterruptVector(1)` | 1 | Immediate. 0xFED08000 (zweiter Controller) kommt nicht als Konstante vor. |
| I/O-Port 0x80C0 | D3D (siehe Abschnitt 3) | 3 | in/out |
| Kernel-Image 0x8002xxxx–0x8003xxxx | XAPI FUN_000f9804 (siehe Abschnitt 3) | 4 | berechnete Adressen |
| NIC 0xFEF00000, Flash 0xFF000000+, PCI 0xCF8/0xCFC, SMBus 0xC000 | – | 0 | PCI-Config nur über `HalReadWritePCISpace` (ord 46) in `CMiniport_InitHardware`, SMBus nicht direkt |

**Falsch-Positive, ausdrücklich geprüft:**
- XPP 0x28d295/0x28f41f `mov eax,0xfd050f80`: `KeSetTimer`-DueTime −50 000 000 (5 s relativ, `or ecx,-1` als High-Teil, 0x28d291).
- `0xFEEEFEEE` in den XAPI-Heap-Funktionen ist ein Füllmuster (`RtlCompareMemoryUlong`).
- `0xFE770FA6` im Spiel FUN_00100c50 ist eine Konstante.
- `.rdata`/`.data` enthalten keine MMIO-Zeiger: 2 Zufallstreffer `0xff00ff00` und `0xfe817e31` (`cpu_datascan.py`).

**Spielcode und NetImmerse:** keine MMIO-, Port-, phys-Fenster- oder Kernel-Adressen. Die 8 `or reg,0x80000000` in FUN_000a37b0/a3b10/a3ce0/a3eb0/a4020/a42a0 setzen ein Flag-Bit (Bit 31) im Blockheader des Spiel-Speichermanagers; dass es „belegt“ bedeutet, ist eine Vermutung. Belegt ist das Muster durch das Decompilat von FUN_000a37b0: `*(uVar1-4) = size | flags&1 | 0x80000000`, daneben `& 0x7fffffff`. Die übrigen 0x8000000x-Immediates im Spielcode sind keine Adressen: `and eax,0x8000000N` gehört zum MSVC-Idiom für vorzeichenbehaftetes Modulo, `push 0x80000000` ist GENERIC_READ (z. B. 0x13f3fa vor FUN_000f76e0), dazu HRESULT-Vergleiche 0x8007000E/0x80004005 (`verify_cpu_hw.py`).

**Interrupt-Service-Routinen** (KeInitializeInterrupt/KeConnectInterrupt im Decompilat):

| Lib | IRQ (HalGetInterruptVector) | ISR | DPC |
|---|---|---|---|
| D3D | 3 | FUN_0023b790 | FUN_0023c1d0 |
| DSOUND (APU) | 5 | LAB_0024b092 | LAB_0024ad32 |
| DSOUND (AC97) | 6 | LAB_002509fd | LAB_00250a0e |
| XPP (OHCI) | 1 (FUN_0028c8f0) | FUN_002920ff (`KeInitializeInterrupt` im XPP-Decompilat, Zeile 649) | FUN_002927d3 u. a. |

Die HLE darf diese Interrupts nie verbinden: `KeConnectInterrupt` liefert FALSE, oder die Bibliotheken sind bereits ersetzt.

**Option für den Loader** (Vermutung, zur Laufzeit per `VirtualAlloc(MEM_RESERVE)` prüfen): Mit `/LARGEADDRESSAWARE` unter WoW64 0xFD000000–0xFEFFFFFF als `PAGE_NOACCESS` reservieren. Dann löst jeder vergessene MMIO-Zugriff eine klar protokollierbare Access Violation aus, statt Speicher zu beschädigen.

---

## 5. Selbstmodifizierender Code, Adressabhängigkeiten, Sektionen

- **Selbstmodifizierender Code:** `cpu_smc.py` findet **0** absolute Schreibzugriffe nach `.text` oder auf disassemblierte Befehle. Der einzige Code-Patcher ist FUN_000f9804, und der schreibt in den **Kernel** (siehe Abschnitt 3).
  - Nachtrag der Verifikation (`verify_cpu_smc.py`, eigener Body-Sweep): Es gibt sehr wohl absolute Schreibzugriffe in die Bibliothekssektionen, aber nur auf deren Daten. Die Ziele liegen in D3D 0x243894–0x246cf4, also komplett im BSS hinter RawSize (allein 0x2444c8 wird 106-mal beschrieben), in DSOUND 0x263a98–0x263adc (raw) und 0x264110–0x264350 (BSS), in XMV 0x28bec0/0x28bec4 (BSS) und in XPP 0x28bfe9–0x28c07c (Datenblock am Sektionsanfang). Kein Ziel liegt auf einem Befehl.
  - Ein Ladeproblem folgt daraus: Bei **D3D (vsz 0x12a38 > raw 0xf5d0), DSOUND (0x1d654 > 0x1d40c) und XMV (0x27b6c > 0x27b5c)** muss der Bereich hinter RawSize genullt und beschreibbar gemappt werden, ebenso wie bei `.data`. Auch `.rdata` (16 B) und `DOLBY` (20 B) haben ein kurzes Zero-Fill-Ende.
- **XBE-Header muss an 0x10000 gemappt sein.** Gelesen werden:
  - Entry: 0x10108 HeaderSize, 0x10118 Zertifikat
  - `XapiInitProcess`: 0x10124 InitFlags (InitFlags liest außerdem `XMountUtilityDrive` 0xf7fb9), 0x10134/0x10138 Heap Reserve/Commit
  - `CreateThread`/`CreateFiber`: 0x10130 Default-Stackgröße
  - `XCalculateSignatureBegin(Ex)`, `XapiSelectCachePartition`, `XLaunchNewImageA`, `XapiBootToDash`, FUN_000f902f/FUN_000f90e6: 0x10118
  - FUN_000f9b8e (`XGetSectionHandleA`, Vermutung): 0x1011c/0x10120
  - Der Entry schreibt sogar in das Zertifikat (`mov [ecx],eax` bei 0xf84f3).
- **Section-API:**
  - FUN_000f9b8e sucht per Name in den Section-Headern 0x1036c…0x10564 und gibt den **Zeiger auf den Section-Header als Handle** zurück, sonst −1 und `SetLastError(0x490)`.
  - `XLoadSectionByHandle` (0xf9bf5) ruft `XeLoadSection` (ord 327) und gibt `[h+4]` (VA) zurück. `XFreeSectionByHandle` (0xf9c16) ruft `XeUnloadSection` (ord 328). `XGetSectionSize` (0xf9c34) gibt `[h+8]` zurück.
  - Einzige Nutzer: `XapiMapLetterToDirectory` (0xf8c41) mit `$$XTINFO` (fehlt), `$$XTIMAGE` und `$$XSIMAGE` (fehlt). FUN_000f8b5a öffnet `\TitleImage.xbx` im Titelverzeichnis mit `NtCreateFile` (CreateDisposition 3) und schreibt den Sektionsinhalt nur, wenn die Dateigröße 0 ist (`NtQueryInformationFile`, 0xf8b5a ff.).
  - **HLE:** `$$XTIMAGE` (0x3178c0, 0x2800) und `.XTLID` (0x31a0c0) beim Laden mitmappen. `XeLoadSection` zählt den Referenzzähler hoch und liefert STATUS_SUCCESS, `XeUnloadSection` zählt nur herunter.
- **DOLBY** (Preload-Flag gesetzt, Flags 0x26): Daten für den APU-DSP. Bleibt mit HLE-DSOUND unbenutzt, muss aber gemappt sein, weil DSOUND-Code darauf zeigt (0x250bf0–0x250e3d). FUN_00250d79 kopiert 0x310740 (0x320 B), 0x310a60 (0x5fc), sechs Blöcke aus FUN_00250bc9 und zusätzlich 0x317860 (0x60 B, 0x250e3d; Nachtrag).
- **Code-Sektionen enthalten Daten.** D3D/DSOUND/XMV/XPP haben das Writable-Flag (0x07) und tragen ihre eigenen Globals. D3D/DSOUND/XMV/XPP brauchen RWX, `.text` RX (`.text`-Flags 0x16 = Preload, Executable, Head-Page-RO).
  - Nachtrag: Die Sektionsgrenzen sind **nicht seitenausgerichtet**. Gemeinsame Seiten wie 0x234000 (`.text`/D3D), 0x246000, 0x264000, 0x28b000, 0x293000 (XPP/`.rdata`), 0x2bd000 (`.rdata`/`.data`), 0x310000, 0x317000 und 0x31a000 brauchen die Vereinigung der Rechte. Die letzte `.text`-Seite ist also RWX.
  - `.rdata` muss beim Laden beschreibbar sein, denn die Kernel-Thunk-Tabelle 0x293560 liegt darin.

---

## 6. Konkrete To-dos für den Loader

1. **Mappen:** Header 0x10000 bis 0x10968 (beschreibbar), alle Sektionen einschließlich `$$XTIMAGE` und `.XTLID`. Bei **jeder** Sektion `VirtualSize - RawSize` nullen: `.data` ab 0x2d9438, D3D 0x243890–0x246cf8, DSOUND 0x26410c–0x264354, XMV 0x28bebc–0x28becc, außerdem die kurzen Enden von `.rdata` und `DOLBY` (korrigiert, vorher nur `.data`). Seiten ausführbar; auf gemeinsamen Grenzseiten die Vereinigung der Rechte setzen. Host-EXE mit `/SAFESEH:NO` und `/LARGEADDRESSAWARE`.
2. **TLS-Slots** S_TLS, S_KTHREAD, S_PRCB, S_IRQL, S_DPC früh allozieren (<64) und prüfen. Dann die 38 disp32 aus `fs_sites.tsv` (`offset != 0x0`) patchen, mit Originalbytes-Vergleich.
3. **HLE `PsCreateSystemThreadEx`:**
   - Host-Thread mit Adopt-Prolog (Slots setzen, Fake-KTHREAD, TlsData mit TlsDataSize Byte).
   - Danach `SystemRoutine(StartRoutine, StartContext)` aufrufen, also 0xf99da – genau wie der Xbox-Kernel.
   - Den Entry 0xf84dd auf einem adoptierten Startthread mit TlsData = 0 ausführen, aber mit gültigem Dummy-TLS-Anker (siehe 2.2). Der Entry kehrt nach `CloseHandle` mit `ret` zurück (0xf854e); die HLE muss das als Thread-Ende behandeln.
   - Stackgrößen: Der Main-Thread bekommt 0x10000 (Default aus 0x10130), die einzige Fiber 0x4000. Ersatzfunktionen, die auf diesen Stacks Host-APIs rufen (Datei-I/O aus der Lade-Fiber, Grafik/Audio), brauchen Reserve. Die HLE sollte die Stacks größer anlegen, als angefordert wird (**Vermutung**: der Xbox-Code verlässt sich nicht auf die exakte Größe). `_chkstk` (FUN_00167150) prüft jede Seite (`test [ecx],eax`, 0x167179), und es gibt kein `sub esp,≥0x1000` ohne Probe. Windows-Guard-Pages funktionieren also.
4. **Ersetzen:** XAPI-Fibers (TEB-Stackgrenzen und SEHOP-Terminal pflegen), `QueryPerformanceCounter`/`QueryPerformanceFrequency`, `OutputDebugStringA`. D3D/DSOUND/XPP ohnehin (alle privilegierten Befehle und das gesamte MMIO).
5. **HLE-Werte:**
   - `XboxKrnlVersion.Build` nicht 3944/4034/4039/4817/4818
   - Fake-KPRCB +0x250 = 0
   - IRQL-Slot konsistent mit `KfRaiseIrql`/`KfLowerIrql`/`KeRaiseIrqlToDpcLevel` (ord 160/161/129), plus globales DPC-Lock
6. **Selbsttests beim Start:** TEB-Slot-Offset prüfen, `IsBadReadPtr(0,4)` muss TRUE liefern (SEH-Dispatch), eine C++-Exception über `RaiseException` werfen und fangen.

---

## 7. Offene Punkte

- Thunk-Slot-Namen: 67 von 111 sind über das Decompilat aufgelöst (`cpu_kthunks.py`). Für ord 258 (Ende von `XapiThreadStartup`) und weitere fehlt der Name. Den Abgleich mit dem Kernel-Bericht (`scripts\kernel_*`) übernehmen. Nachtrag: `kernel_imports.tsv` führt ord 258 als `PsTerminateSystemThread`. Das passt zur Aufrufstelle 0xf9a6b mit 1 Push-Argument und `int3` danach (noreturn).
- Ob `RtlIsValidHandler` unter aktuellem Windows Handler in ausführbarem privatem (Nicht-Image-)Speicher akzeptiert und welchen SEHOP-Default der Host-Prozess hat: zur Laufzeit testen (siehe Abschnitt 6).
- Bedeutung von KPRCB+0x250 und Port 0x80C0 ist eine Vermutung; für den Port egal, solange +0x250 = 0 ist und D3D ersetzt wird.
- Die 21 783 Byte `gap-data` in `.text` sind nicht weiter klassifiziert (Jump-/Byte-Tabellen, EH-Daten). Laut `cpu_scan.py --all` enthalten sie gar keinen fs:-Pseudo-Treffer. Alle 409 `gap-data`-Treffer liegen in den Datentabellen von D3D/DSOUND/XMV/XPP. Nur einer davon hat einen absoluten disp (`64 00 25 00000000` bei 0x263dba, DSOUND-Tabelle 0x250ee9–0x26410c), der Rest nutzt Basisregister.

---

## Verifikation

Unabhängige Gegenprüfung des Berichts am Binary (`orig\retail_eu\default.xbe`) und am Ghidra-Export, ohne Ghidra zu starten. Die Prüfskripte importieren `cpu_rd.py`/`cpu_scan.py` nicht; sie nutzen nur `tools\xbe.py`, capstone und die Export-TSVs. Ausnahme ist der bewusste Reproduktionslauf von `cpu_rd.py`/`cpu_gaps.py`. Alle Skripte liegen in `scripts\`, ihre Ausgaben in `scripts\out\`:

| Skript | prüft |
|---|---|
| `verify_cpu_common.py` | gemeinsame Helfer (Ghidra-Körper aus `functions.tsv`, Disassembler) |
| `verify_cpu_fs.py`, `verify_cpu_fs2.py` | Byte-Scan aller 0x64/0x65-Bytes gegen `fs_sites.tsv`, Offsets, Encodings, disp-Position, `func`/`inside`-Zählung, Artefakte außerhalb der Ghidra-Körper |
| `verify_cpu_tls.py` | TLS-Verzeichnis, Sektions-Header/Flags, Entry, Main-Thread, `XapiThreadStartup`, `CreateThread`, `Get/SetLastError`, `_getptd`/`_freeptd`/`_mtinit`, alle `fs:[0x20]`- und `fs:[0x28]`-Folgebefehle |
| `verify_cpu_thunks.py` | Slot → Ordinal direkt aus der Thunk-Tabelle (111 Einträge, alle mit Bit 31), Name als Schnittmenge der EXTERNAL-Callees aller Aufrufer-Funktionen, `RtlUnwind`-Stub, Aufrufer |
| `verify_cpu_fiber.py` | Aufrufer von Fiber-API, QPC/QPF, `OutputDebugStringA`, FUN_0013f250/FUN_0013f590 (callgraph + E8/E9-Byte-Scan) |
| `verify_cpu_priv2.py`, `verify_cpu_classify.py` | privilegierte/Sonderbefehle: (A) Sweep aller Ghidra-Körper, (B) Sweep aller Lücken |
| `verify_cpu_gaptriage.py`, `verify_cpu_rdgap.py` | Einordnung der Lücken-Treffer (Zeigertabelle/Code/Sprungziel); eigene, bewusst großzügige Erreichbarkeitsanalyse für Lückencode |
| `verify_cpu_mmio.py`, `verify_cpu_hw.py` | MMIO/phys/Port-Operanden und Immediates je Sektion und Funktion, Datenscan |
| `verify_cpu_seh.py`, `verify_cpu_seh2.py` | EH-Thunks, eingetragene Handler, `__SEH_prolog` |
| `verify_cpu_smc.py`, `verify_cpu_hdr.py` | absolute Schreibzugriffe in Codesektionen, DOLBY-Wortformat, Header-Zugriffe |
| `verify_cpu_stack.py`, `verify_cpu_stack2.py` | `_chkstk`, große Stackframes, angeforderte Stackgrößen |
| `verify_cpu_callsites.py` | Byte-Scan der Aufrufstellen (FUN_0013f250, `_getptd`, QPC/QPF, Section-API, `__SEH_prolog` …) |
| `verify_cpu_fixtsv.py` | ergänzt die 2 fehlenden APU-Immediates in `cpu_hw_refs.tsv` |

### Kernaussagen

| # | Aussage | Ergebnis | Beleg |
|---|---|---|---|
| 1 | 5206 fs:-Stellen, 6 Offsets, kein 0x18/0x2c/0x34, kein gs: | **bestätigt** | Alle 5206 Zeilen stimmen byte-genau mit dem XBE überein. Der Byte-Scan findet jede Stelle wieder. Jede fs:-Stelle, die der lineare Sweep der Ghidra-Körper auf einer Befehlsgrenze trifft, steht in der TSV (4879/4879). Absolute fs/gs-Kandidaten außerhalb der TSV sind Artefakte mitten in Befehlen, z. B. 0x1dac8 in `call 0xefb30`, 0xab34f in `fstp [ebx+0x64]`, 0x17c80e in `jmp 0x166977`, oder Daten (0x263dba). Basisrelative fs:-Treffer liegen ebenfalls mitten in Befehlen (Stichprobe von 10, alle `[reg+0x64]`-Operanden). |
| 2 | fs:[0] in 1486 Funktionen, nur absolute disp32, keine in D3D/DSOUND/XMV/XPP | **korrigiert** | 1486 ist die Zahl verschiedener `func`-Werte, keine Funktionsanzahl: 1419 Ghidra-Funktionen (4878 Stellen) plus 67 `?(after …)`-Werte (290 Stellen hinter zu kurzen Ghidra-Körpern). Die Zahlen in Klammern (4202/882/51/31/2) sind Stellen, keine Funktionen. Je Bibliothek sind es 1197/260/17/11/1 verschiedene `func`-Werte. Encodings ×2163/×1495/×1493 bestätigt, alle 5168 in `.text`. |
| 3 | `_tls_index` = −9, Zugriff `[[fs:4]−0x24]` | **bestätigt** | Entry 0xf84f5–0xf852f, nachgerechnet: ((0x20+0−0+0xF)&~0xF)+4 = 0x24, 0x24/−4 = −9. Index-Variable 0x2f3d18 liegt im `.data`-BSS. |
| 4 | TLS-Block +4/+8/+0xC/+0x10 | **bestätigt** | 0xf6e75/0xf6ea1, 0x169301/0x169499, 0x17096e/0x17097a, 0x1709a1 |
| 5 | KTHREAD nur +0x1C/+0x28/+0x12C, KPRCB nur +0x250 | **bestätigt** | Alle 9 `fs:[0x28]`-Folgebefehle (2 davon tote Loads in Get/SetLastError) und alle 8 `fs:[0x20]`-Stellen geprüft. Codes 0x0a/0x0b/0x0b/0x0a/2/0x0c stimmen. |
| 6 | 38 Nicht-Null-Stellen gleich lang auf `fs:[0xE10+4k]` umschreibbar | **bestätigt** | capstone-`disp_offset` stimmt bei 38/38 mit +2/+3/+4 überein. Encodings: `64 A1` ×19, `64 8B 0D` ×9, `64 8B 3D` ×2, `64 0F B6 05` ×8. |
| 7 | Ghidra-only-Scan findet 29 statt 38; 9 fehlende | **bestätigt** | Liste identisch (0xf6384, 0xf8473, 0xf848f, 0xf8495, 0xf99ea, 0x16935e, 0x169372, 0x169383, 0x170880). 0x169352 ist `_freeptd` (Freigaben, Slot nullen, 1 Aufrufer 0x16ed15), nicht eine „zweite `_getptd`-Variante“. |
| 8 | Fibers als Tasks; SwitchToFiber tauscht nur esp/fs:[0]; ExceptionList −1 | **bestätigt, präzisiert** | `SwitchToFiber`/`CreateFiber` wie beschrieben. `callgraph.tsv` nennt **27** Aufrufer von FUN_0013f250 (nicht „über 30“); der Byte-Scan findet 35 Stellen in 28 Funktionen. FUN_0013f250 ist ein Datei-/Stream-Konstruktor mit genau einer 16-KB-Lade-Fiber (Text in 2.5 korrigiert). |
| 9 | QPF fest 733 333 333, QPC = rdtsc | **bestätigt** | 0xf6434 `c7 00 55 c7 b5 2b`, 0xf641f `0f 31`. Die „733“-Treffer der Konstantensuche (0x17704, 0x2fae8, 0x150297) sind jcc-rel32-Bytes. [0x2939f8] = 1.0f. |
| 10 | Nur 9 privilegierte Befehle in echtem Code | **bestätigt** | Der Körper-Sweep findet genau diese 9 (plus Tabellen-Artefakte in `_memmove` und FUN_00241c40: `arpl`, `push ss`, `push cs`, `bound`, `pushal`). Die Lücken-Treffer sind Sprungtabellen hinter `ret` (z. B. 0x9dc98, 0x104298, 0x10ff4c, 0x23bf58, 0x837a0), Befehlsreste hinter zu kurzen Ghidra-Körpern (z. B. `e5 5d` = Rest von `8b e5 5d` an 0x77869, 0xa9047, 0xc7ca1, 0xc830c, 0xd1e25) oder Datentabellen. Kein `lock`-Präfix in Körpern. |
| 11 | Kernel-Patcher FUN_000f9804 nur bei Build 3944/4034/4039/4817/4818 | **bestätigt** | Adresstabelle je Build und `XboxHardwareInfo`-Bit 1 nachgerechnet (0x8002ABBA/0x80039749, 0x8002AB88/–, 0x8002AB98/0x800398CD, 0x800299A0/0x8003838F). Ord 179 bekommt 1 Argument (Query), ord 182 3 Argumente (Set). Hinweis: Bei passendem Build liest die Funktion die Kernel-Adresse (0xf9883) vor dem Patch; unter WoW64 wäre das eine Access Violation. Die Kernaussage nennt nur 4 der 7 Adressen, Abschnitt 3 ist vollständig. |
| 12 | MMIO nur in DSOUND/D3D/XPP, 266 APU-Zugriffe in 43 Funktionen | **korrigiert (Zählung)** | 266 Speicheroperanden in **42** Funktionen (257 absolut + 9 basisrelativ), dazu **4** Immediates statt 2 (0x24a1f9, 0x24cb6b fehlten in der TSV, jetzt ergänzt). Zusammen ergeben sich 43 Funktionen. AC97 22 in 11 Funktionen (9 absolut + 13 basisrelativ), NV2A 1 Immediate + 7 Zugriffe (0x23bcbb liegt hinter dem zu kurzen Körper von FUN_0023bcb0) bestätigt. |
| 13 | Spiel/NetImmerse ohne Hardwarezugriffe | **bestätigt** | Keine Speicheroperanden in MMIO/phys-Bereichen in `.text`. `.text`-Immediates im FE/FF-Bereich sind Konstanten (0xFEEEFEEE, 0xFE770FA6, Farb-/Maskenwerte). Das Decompilat von FUN_000a37b0 stimmt mit dem Bericht überein. |
| 14 | Kein `lock`, keine Interlocked-Importe | **bestätigt** | 0 `lock` in Körpern. Im Decompilat gibt es 0× „Interlocked“; das einzige `LOCK()` ist Ghidras Darstellung des implizit atomaren `xchg [esp],eax` (0x166920). `KeRaiseIrqlToDpcLevel`: 23 Slot-Referenzen im Binary, 23 Aufrufe im Decompilat. |
| 15 | Kein SMC; Section-Handles = Header-Zeiger; nur `$$XTIMAGE` | **bestätigt, ergänzt** | Kein absoluter Schreibzugriff nach `.text` oder auf Befehle. Absolute Schreibzugriffe in D3D/DSOUND/XMV/XPP treffen deren Daten/BSS (ergänzt in Abschnitt 5). FUN_000f9b8e (RtlInitAnsiString/RtlEqualString-Schleife, −1 + `SetLastError(0x490)`), `[h+4]`, `[h+8]` und ord 327/328 bestätigt. FUN_000f8b5a: `NtCreateFile` mit Disposition 3, Größenprüfung über `NtQueryInformationFile` Klasse 0x22. |
| 16 | DOLBY = 24-Bit-DSP-Wörter | **bestätigt** | 7259/7259 Dwords mit Nullbyte an VA%4 = 3. Kopierquellen bestätigt, zusätzlich Block 0x317860 (0x60 B). |
| 17 | 1238 `___CxxFrameHandler`-Thunks; `__SEH_prolog` 42 Aufrufer, Handler 0x168dcc; ord 302/312 | **korrigiert** | 1117 Thunks (eigene Zählung), mindestens 1477 bzw. 1488 Registrierungen. `__SEH_prolog` hat 42 E8-Aufrufer und trägt 0x168dcc ein. Slot 0x2935a0 = ord 302, Slot 0x293704 = ord 312, Stub 0x170aae, Aufrufer `_UnwindNestedFrames` und `__global_unwind2` mit je 4 Push-Argumenten. |

### Weitere geprüfte Einzelaussagen (alle bestätigt, sofern nicht anders vermerkt)

1. Sektionstabelle: Flags `.text` 0x16, D3D/DSOUND/XMV/XPP/`.data` 0x07, `.rdata` 0x06, DOLBY 0x26, `$$XTIMAGE`/`.XTLID` 0x38 (kein Preload). Rohgrößen wie in Abschnitt 1. Ein Neulauf von `cpu_rd.py`/`cpu_gaps.py` reproduziert 664 613 Befehle und die Byte-Klassen. Die Seed-Zahlen sind 2636/727 statt 2635/728 (korrigiert); 5 246 B in `.text` sind Sprungtabellen (ergänzt).
2. `CreateThread` → `PsCreateSystemThreadEx` (Slot 0x293694 = ord 255): 10 Pushes, Arg 4 = `[0x310394]`, Arg 10 = 0xf99da, Default-Stack `[0x10130]` = 0x10000, `ret 0x18`.
3. `XapiThreadStartup`: `TlsData[0] = TlsData+4`, Kopie/Zero-Fill, `call [ebp+8]`, `call [0x293690]` (ord 258) + `int3`. Ergänzt um die Notify-Aufrufe 0xf98b5.
4. `GetLastError`/`SetLastError`: Der `fs:[0x28]`-Load ist auf beiden Pfaden tot, eine TlsData-Prüfung fehlt.
5. `_getptd`/`_freeptd`: IRQL ≥ 2 → `KeBugCheck(0xa)` (Slot 0x293708 = ord 95). Globaler Slot 0x310734 bei TlsData = 0. `_mtinit` schreibt `[TLS+8]` ohne Prüfung.
6. Main-Thread 0xf8469: `call 0xf9804`, `call 0xf92c0` (XapiInitProcess), Debug-Hook `[p+0x24]`, `call 0xf97db`, `call 0xf9783` (`_cinit`), `call 0xd29b0`, danach `call 0xf8fca(1,1,0)`. Die Kette ist im Disassembly bestätigt.
7. DSOUND: 0x246d37 vergleicht mit 2 und ruft `KfRaiseIrql` (ord 160, fastcall `cl=2`). 0x246e0d: IRQL = 0 → `RtlEnterCriticalSection(0x263b20)`. `fs:[0x58]` schaltet ord 142/139 mit Zähler 0x263ad0.
8. `CreateFiber`: Minimum 0x3000, Seitenrundung, `MmCreateKernelStack` (ord 169), `[f+4]`/`[f+8]`, Startframe mit ExceptionList −1. Die Startroutine 0x17086b ruft `KeBugCheck(0)`, wenn die Fiber-Funktion zurückkehrt. `DeleteFiber` → ord 170 (`[f+4]`, `[f+8]`). `ConvertThreadToFiber` `and [ecx+8],0`.
9. QPC-Aufrufer (7 Stellen/5 Besitzer) und QPF-Aufrufer (4) wie angegeben. `OutputDebugStringA`: `eax=1`, `ecx=&ANSI_STRING`, `int 0x2d`, `int3`; 2 Aufrufer.
10. cpuid-Bug (`mov edx,0x80000001` statt eax) bestätigt. `[0x30ef44]` liest nur FUN_00151150 in eine lokale Variable; der Wert ist nicht der Rückgabewert.
11. ISR/DPC-Tabelle: IRQ 3/5/6/1, ISR 0x23b790/0x24b092/0x2509fd/0x2920ff, DPC 0x23c1d0/0x24ad32/0x250a0e. XPP-DPC 0x2927d3 (`KeInitializeDpc` 0x28c98a) ruft FUN_002921b9 (cli/sti, 0xFE80200C).
12. XPP `mov eax,0xfd050f80` + `or ecx,-1` → `KeSetTimer` (Slot 0x293650 = ord 149) mit DueTime −50 000 000.
13. Datenscan `.rdata`/`.data`: nur 0xff00ff00 (0x2a18b4) und 0xfe817e31 (0x2d8e1c) in Geräte-Bereichen.
14. Header-Zugriffe (0x10108, 0x10118 ×14, 0x1011c/0x10120, 0x10124 ×6, 0x10130 ×2, 0x10134/0x10138). Der Entry schreibt bei 0xf84f3 ins Zertifikat. Das Immediate 0x105b8 in D3D FUN_0023ef0d ist eine Registerkonstante, kein Header-Zeiger.
15. TEB-Offsets der Strategie A (0xE10 TlsSlots, 0xE0C DeallocationStack, 0x20/0x24/0x28/0x58) entsprechen dem dokumentierten x86-TEB. Das Beispiel `64 A1 2C 0E 00 00` für Slot 7 ist korrekt (0xE10 + 0x1C).

### Korrekturen im Text

- 0: fs:[0]-Verteilung (1419 + 290 Stellen statt „1417 + 69“), APU-Immediates, neue Punkte 6 (BSS, beschreibbarer Header) und 7 (nur zwei Threads).
- 1: Seed-Zahlen und Sprungtabellen-Bytes.
- 2.1/2.5: Thunk-Anzahl 1117 statt 1238; Handler-Nachzählung.
- 2.2/2.4/6: Entry-Thread braucht einen gültigen Dummy-TLS-Anker; Notify-Aufrufe in `XapiThreadStartup`; Rückkehr des Entry.
- 2.3/2.5: FUN_0013f250 ist kein „Task-Konstruktor“, sondern ein Datei-/Stream-Konstruktor mit einer Lade-Fiber (16 KB); `ConvertThreadToFiber` nur einmal (Flag 0x30e594). `RtlRaiseException` wird auch aus den XAPI-Heap-Funktionen gerufen.
- 3: Nicht alle `int3` sind unerreichbar: 3 D3D-Stellen sind Fehlerpfad-Breakpoints.
- 4: APU-Zählung (42 + 1 Funktionen, 9 basisrelative Operanden, 4 Immediates); `cpu_hw_refs.tsv` um 2 Zeilen ergänzt (jetzt 332); `and reg,0x0FFFFFFF`-Umrechnungen ergänzt; „belegt“-Bit als Vermutung markiert.
- 5/6: BSS von D3D/DSOUND/XMV (und Zero-Fill-Enden von `.rdata`/`DOLBY`) muss genullt werden; gemeinsame Grenzseiten; `.rdata` beim Laden beschreibbar; DOLBY-Block 0x317860; `XMountUtilityDrive` liest InitFlags; Stackgrößen und `_chkstk`.

`fs_sites.tsv` brauchte keine Korrektur (5206 Zeilen byte-genau, Offsets/disp-Positionen korrekt). `cpu_priv_sites.tsv` bleibt unverändert; die neue Bewertung der 3 D3D-`int3` steht nur im Text.

### Offen / unsicher

- Ob wirklich **jeder** Lückencode gefunden ist, lässt sich ohne vollständige Kontrollfluss-Rekonstruktion nicht beweisen. Die eigene großzügige Erreichbarkeitsanalyse (`verify_cpu_rdgap.py`) seedet über Immediates und Daten-Dwords und erreicht dadurch auch Datentabellen. Ihre Treffer wurden deshalb einzeln bzw. nach Regionen eingeordnet (`verify_cpu_gaptriage.py`). Ein privilegierter Befehl in echtem Code außerhalb der 9 bekannten wurde nicht gefunden.
- Die Gesamtzahl der Handler-Registrierungen mit C++-EH-Thunk hängt vom Suchfenster ab (1477 bis 1488). Für den Port ist das ohne Belang.
- Bedeutungen von KPRCB+0x250, `fs:[0x58]`, Port 0x80C0 und `XboxHardwareInfo`-Bit 1 bleiben Vermutungen. Bit 1 ist nach öffentlichen xboxkrnl-Headern wohl das Devkit-Kernel-Flag (**Vermutung**, nicht am Binary belegbar).
- Verhalten von `int 0x2d`, SEHOP-Default und `RtlIsValidHandler` unter WoW64 ist nicht am Binary prüfbar und muss zur Laufzeit getestet werden.
- Wie viel Stack Host-Ersatzfunktionen auf den 64-KB- bzw. 16-KB-Stacks brauchen, ist offen (Vermutung: deutlich mehr als 16 KB bei Grafik-/Audio-Treibern).
