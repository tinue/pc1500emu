# Changelog

All notable changes to this project are documented here. Versions follow
`CMakeLists.txt`'s `project(pc1500emu VERSION ...)`, bumped on every push
per this project's own convention (not just milestones).

## [0.7.5] - 2026-08-20

### Fixed
- `setce168n`'s bank-select trigger now follows the machine variant, same
  as CE-163's own trigger: `5800H`-`5FFFH` on a PC-1500, `6800H`-`6FFFH` on
  a PC-1500A. Previously hardcoded to the PC-1500 range regardless of
  `setmachine`, so CE-168N silently stopped responding to bank-select
  writes on a PC-1500A.

## [0.7.4] - 2026-08-18

### Added
- `setce168n <banks> <firstRoBank>` FIFO command: a generalized CE-163 --
  same `0000H`-`3FFFH` window and `5800H`-`5FFFH` bank-select trigger, but
  with the bank count and a first-read-only-bank boundary as load-time
  parameters instead of CE-163's fixed 2 banks/all-writable. Banks at or
  above the boundary simulate flash: CPU writes (and `poke`) are silently
  discarded, but `loadbinary` can still seed their initial content. FIFO
  interface only, no GUI menu equivalent.

## [0.7.3] - 2026-08-21

### Fixed
- `ERROR 13` on RESERVE-key assignment could still occur under a non-bare
  RAM configuration even with 0.7.2's `reseedReserveArea()` fix, if the
  saved state file being auto-loaded on startup predated that fix (or was
  saved under a different RAM configuration): `Bus::loadState` restored
  `me0_` and the RAM-config scalars directly, bypassing the setters (and
  therefore `reseedReserveArea()`) entirely. `Bus::loadState` now refuses
  to load a state file whose saved RAM configuration doesn't match the
  Bus's current configuration at load time, leaving the Bus completely
  untouched -- raw RAM contents (including the reserve area) are only
  meaningful relative to the specific memory-map shape they were saved
  under, the same way real PC-1500 RAM (short of a battery-backed module)
  doesn't survive a hardware reconfiguration either. `main.cpp`'s startup
  sequence now applies `AppConfig`'s persisted hardware settings
  unconditionally, before attempting any state-file load, so there's
  always a well-defined "current config" for this check.

### Added
- Rather than silently discarding a saved session that fails the new
  config-mismatch check above, `pc1500emu` now shows a startup popup
  explaining what didn't match and offering to apply the saved
  configuration and retry the restore, in addition to just starting fresh
  with the current settings -- the session is still sitting right there
  in the state file, just not currently loadable.

### Changed
- Help > Special Keys' Tab row shortened to "SHIFT (toggles SHIFT and
  clears on next keypress)" -- clearer about which physical key is meant
  and less text than 0.7.2's rewording.

## [0.7.2] - 2026-08-21

### Fixed
- The PC-1500A's F1-F6 "reserve key" assignment feature (`SHIFT+MODE`)
  threw `ERROR 13` ("insufficient space for RESERVE") under any non-bare
  RAM configuration (e.g. 16K at `0000H`, CE-155). Root cause: the
  reserve area's required zero-seeding (the ROM never initializes it
  itself -- see the 2026-07-26 fix below) was a one-time, hardcoded
  `4008H`-`40C4H` applied only at `Bus` construction, never re-applied
  when the live reserve area moves along with `basicProgramStart()`'s own
  RAM-config-dependent origin. `Bus::reserveAreaBase()`/
  `reseedReserveArea()` now compute and re-seed the correct live location
  on construction and on every RAM-config-changing setter.
- Text sent through any FIFO/pipe text-typing command (`type`,
  `typeline`/`typelinetrace`/`typelinenoenter`/`typelinepartialidle`/
  `typelinepartialidletrace`/`typelinewatch`, `loadbasictext`) with SML
  (lowercase mode) active could silently drop spaces, digits, and
  punctuation immediately following a case change -- the shared
  `SmlAwareTyper` helper was toggling SML around *every* character whose
  literal-lowercase-ness didn't match the current mode, including
  characters with no case-dependent representation at all. Real/live
  keyboard typing never had this bug (a human only presses SML around an
  actual letter). Now scoped to letters only.
- The PC-1500A has real, independent 1K RAM at `7C00H`-`7FFFH`, unlike
  the base PC-1500 (a mirror of `7800H`-`7BFFH`, unchanged).
  `Bus::effectiveAddr` is now machine-variant-aware for this range.
- The in-app Help > Special Keys popup implied host Shift itself toggles
  PC-1500 SHIFT -- it doesn't (only Tab does, host Shift is inert); the
  table's wording was just misleading, not the underlying keyboard
  handling. Reworded.

### Changed
- Extracted the previously-duplicated SML-tracking logic (independently
  reimplemented in `main.cpp`'s `type` handler and
  `typeBasicProgramText`) into one shared `SmlAwareTyper` class
  (`src/basic/text_loader.h`), now used by every text-sending path
  instead of two copies plus several gaps.
## [0.6.6] - 2026-08-11

### Added
- `pc1500disasm` now annotates individual bits of `STATUS1`/`STATUS2`
  (`764EH`/`764FH`) when a `bii`/`ani`/`ori` instruction's immediate mask
  touches one, e.g. `bii (0x764E),0x02` now shows `[bit: SHIFT]` alongside
  the byte-level name, instead of every instruction touching that byte
  showing an identical generic annotation. Bit names are from the PC-1500
  Technical Reference Manual's own bit-layout table (p.98).

## [0.6.5] - 2026-08-11

### Fixed
- `NUMCMP` (numeric comparison entry point) was cataloged at `D9D2H` --
  corrected to `D0D2H` (likely a 9/0 transcription error in the original
  source), confirmed against both a real `ROM1.BIN` disassembly and the
  PC-1500 Technical Reference Manual's own system-subroutine table.
- Two `CE-150` printer entry points were mislabeled: `A8DDH` (was
  `PRT_LF`, is actually printer motor drive) and `AA09H` (was
  `PRT_PEN_UPDOWN`, an address the manual doesn't list at all) -- the
  real `PRT_LF` is `A9F1H` and the real `PRT_PEN_UPDOWN` is `AAE3H`, both
  confirmed against this project's own `CE-150.ROM`.
- The cassette/printer `CE-150` module entries' own comments said they
  load at `8000H` (per the PC-2 Assembly manual's prose) -- corrected to
  `A000H`, confirmed against both the Technical Reference Manual's own
  memory map and this project's `CE-150.ROM` disassembly.

### Added
- Three new confirmed `CE-150`/ROM entry points from the PC-1500
  Technical Reference Manual's own system-subroutine table (p.120-121):
  `PRT_TEXT_MODE` (`ACBBH`), `DISP_GRAPHIC` (`EDEFH`), and
  `TAPE_IO_CONTROL` (`BBF5H`).

## [0.6.4] - 2026-08-11

### Added
- `pc1500disasm` now annotates the BASIC interpreter's own named RAM
  variables (WAIT counter, FOR/GOSUB pointers, current/previous/search/
  break/error line+address+top fields, ON ERROR GOTO target, USING format
  state, pen-plotter/printer variables) in `7800H`-`7BFFH`, transcribed
  from the PC-1500 Technical Reference Manual's own table (pp.100-101) --
  e.g. `ori (0x78B8),0x80` now shows `; ON_ERROR_ADDRESS_H -- ...`.

## [0.6.3] - 2026-08-11

### Fixed
- BREAK (F12) stopped working to interrupt a running program. The MI
  interrupt handler itself reads the IF register to check an unrelated
  bit as part of its own dispatch logic, and an earlier fix cleared
  BREAK's own flag as an unintended side effect of *any* read of that
  register -- so every BREAK-triggered interrupt silently consumed its
  own flag before the interpreter's break-check ever saw it.
- Several less-common ways of driving the CPU (the `break`/`run`/`trace`
  FIFO commands, debugger single-stepping, and a few internal keystroke-
  typing helpers) didn't advance the real-time clock the same way the
  main loop does, so a `WAIT`/`BEEP` in progress during one of those could
  stall or run at the wrong rate.
- Escape now dismisses the "Special Keys" and "About" dialogs, matching
  every other dialog.

### Added
- `--no-state` command-line flag: boot cold this run without touching the
  configured state file (skips both auto-load and auto-save-on-exit for
  the session), for testing/reproduction runs that need a known starting
  point.

## [0.6.2] - 2026-08-11

### Fixed
- `WAIT n` (and BEEP's gap-timer) could run many times slower than its
  true `n/64` seconds -- confirmed live (WAIT 64 took ~10 real seconds
  instead of 1). A rendered frame's entire CPU cycle budget executes in
  well under a millisecond of real host time, so reading the real clock
  directly on every register access meant WAIT's poll loop saw the same
  frozen timestamp for an entire frame's burst, missing most RTC ticks.
  Fixed by advancing the RTC's clock smoothly per instruction (scaled by
  its own cycle cost), re-anchored to the real clock once per frame.
- After any WAIT/BEEP had run once, the screen could clear spuriously at
  the idle READY prompt from then on, for the rest of the session -- the
  real-time clock's TP output never stopped oscillating once configured
  (nothing ever turned it back off), so its ticks kept leaking into a bit
  shared with BREAK detection, which the idle loop misread as BREAK
  presses. Fixed by disabling TP once WAIT/BEEP's own poll loop is done
  with it.

## [0.6.1] - 2026-08-11

### Added
- `pc1500disasm` now cross-references BASIC keyword-table entries back
  onto their implementation address (e.g. `LE86AH:  ; WAIT keyword`),
  instead of requiring a manual keyword-table lookup to identify a
  keyword's own entry point in a listing.

### Fixed
- `WAIT n` (and BEEP's identical gap-timer) could exit far too early with
  a spurious "BREAK IN <line>" error, or otherwise complete noticeably
  faster than its requested duration, due to a hardware-timing race
  between two status registers (`OPB`/`IF`) both derived from the
  real-time clock's tick signal. `WAIT n` now counts down its full
  duration at the correct, linear 64Hz rate.
- "Load BASIC Text": a long line whose greedy per-pass packing happened to
  land mid-parenthesized-expression (e.g. right after the "(" in `A$(X,Y)`)
  was rejected outright by the ROM's tokenizer and failed the whole load,
  instead of being recognized as a rejection and retried with a shorter
  pass. See `docs/pc1500_hardware_reference.md`'s "BASIC line editor"
  section for the confirmed ROM behavior this is built on.
- "Load BASIC Text": a validation error message could run off the edge of
  the dialog instead of wrapping.

### Removed
- "Load BASIC Text": the redundant "Paste from Clipboard" button (the text
  box already supports the platform's normal paste shortcut).

## [0.6.0] - 2026-08-09

### Added
- "Load BASIC Text" now supports source lines longer than the ROM's
  79-character raw-input limit, using the same multi-pass LIST-and-append
  technique real PC-1500 owners used: type up to the ROM's own limit,
  Enter, then resume editing the same line to append more, repeating as
  needed. See `docs/pc1500_hardware_reference.md`'s "BASIC line editor"
  section for the confirmed mechanics this is built on.
- "Load BASIC Text" shows a "Loading..." indicator (and a wait cursor)
  while a long listing is being typed in, instead of appearing to hang.
- Enter/Escape keyboard shortcuts for dialogs: Enter triggers the primary
  action (Load, Save, etc.) on every dialog except "Load BASIC Text";
  Escape triggers Cancel on all dialogs. Escape also closes an open menu.
- Keyboard shortcuts for actions that previously required the mouse:
  Ctrl+Alt+O/S for Load/Save BASIC Text, Ctrl+Alt+A for Automation Mode,
  Ctrl+Alt+P for the status panel.
- Extension RAM size (both the 4800H and 0000H windows) is now remembered
  across restarts, via the same conf file as the other Settings-menu
  options.

### Fixed
- macOS: indicator font path no longer hardcodes a Linux-only location;
  tries a candidate list of real macOS system fonts instead.
- macOS: the app window now requests foreground activation on launch,
  instead of opening behind other windows.
- Checking "Auto-Save State on Exit" with no state file ever explicitly
  loaded/saved now defaults to `pc1500emu.state` in the current directory,
  instead of silently doing nothing at exit.
- "Load BASIC Text": the button that reads the Filename field's path into
  the text box is now labeled "Read File" (previously "Load File into
  Text") and stays disabled until a filename is entered. Clicking Load
  after typing or pasting a filename, without pressing Read File first,
  now reads that file automatically.

## [0.5.1] - 2026-08-09

### Fixed
- macOS build/run fixes (font path, window activation, auto-save-on-exit).

## [0.5.0] and earlier

Versions before 0.5.1 were not individually tracked in this file. See
`git log` for the full history -- notable earlier work includes the
LH5801 CPU/bus/keyboard/LCD emulation core, BASIC program load/save
(binary and text), interactive DAP debugger, LH5801 disassembler, and
save-state support.
