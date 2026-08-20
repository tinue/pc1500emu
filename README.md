# pc1500emu

A from-scratch emulator for the Sharp PC-1500 pocket computer, built around
the Sharp LH5801 CPU.

Author: Paul Chambre using Claude Code and CLion

Scope for the initial version:
- LH5801 CPU core (full instruction set)
- PC-1500 memory map and bus I/O
- Keyboard matrix input
- Dot-matrix LCD output (156x7)

Out of scope for now: cassette interface.

This project exists to support later work adding LH5801/PC-1500 target
support to [SDCC](https://sdcc.sourceforge.net/) — this emulator is used to
validate hand-written and assembler/compiler-generated code before any
compiler backend work begins.

## Building

Requires SDL2 and SDL2_ttf development packages. The Linux and Windows
blocks below are alternatives for the two platforms, not sequential steps —
running both against the same `build` directory leaves it stuck on
whichever configuration ran first (see the Windows section's own note).

**Linux**: `libsdl2-dev`, `libsdl2-ttf-dev` on Debian/Ubuntu.
[Dear ImGui](https://github.com/ocornut/imgui) (the menu bar UI) is vendored
directly in `third_party/imgui/` — nothing extra to install for that.

```sh
cmake -B build
cmake --build build
```

**Windows**: install SDL2 and SDL2-ttf via [vcpkg](https://github.com/microsoft/vcpkg)
(`vcpkg install sdl2 sdl2-ttf`), then point CMake at its toolchain file. In
PowerShell, quote the whole `-DCMAKE_TOOLCHAIN_FILE=...` argument — passed
unquoted, PowerShell mangles a value containing `../` before it reaches
`cmake.exe` (it silently drops the `-DCMAKE_TOOLCHAIN_FILE=` prefix, so
CMake sees the path alone and misreads it as the source directory instead,
failing with "is a file, not a directory"):

```powershell
cmake -B build "-DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config RelWithDebInfo
```

`CMAKE_TOOLCHAIN_FILE` only takes effect on a build directory's *first*
configure — CMake processes it before any of this project's own
`CMakeLists.txt` runs, so setting it against a `build` directory that's
already been configured (e.g. the plain `cmake -B build` from the Linux
block above, run against the same folder by habit) has no effect: CMake
either silently ignores it (`CMake Warning (unused-cli): ... variables
were not used by the project: CMAKE_TOOLCHAIN_FILE`) or, if that earlier
configure didn't already find SDL2 some other way, fails to find it at
all. If you hit either of these, delete the `build` directory and
reconfigure from scratch with the toolchain flag present from the start.

The indicator row (see the Keyboard mapping section below) needs a
CJK-capable font for its katakana glyphs; Windows ships one
(`C:\Windows\Fonts\msgothic.ttc`) by default since Windows 8, so nothing
extra to install there either.

## Running

```sh
./build/src/host/pc1500emu [rom.bin]          # Linux
build\src\host\RelWithDebInfo\pc1500emu.exe [rom.bin]   # Windows
```

No PC-1500 ROM dump is bundled (it's copyrighted Sharp firmware) — without
one, the emulator runs with a zero-filled ROM area, which spins harmlessly
rather than doing anything useful. Pass a real dump's path to load it at
`C000H`.

Keyboard mapping (host key -> PC-1500 key) is defined in `src/host/main.cpp`
(`kKeyMap[]`). Letters, numpad digits, arrows, F1-F6, Enter, and Space map
directly; Backspace duplicates the left arrowhead. PC-1500 keys without an
obvious host equivalent live on F7-F12 for muscle-memory reasons (chosen
over keys like Delete/Insert, which don't map intuitively to a
calculator's special keys):
- F7 = CL, F8 = MODE, F9 = DEF, F10 = SML, F11 = RCL
- Shift+F10 = the up/down rocker key
- **F12 = On** (it's wired directly to the CPU, not part of the keyboard
  matrix, so it's handled separately from every other key). While a
  program is running, the same key doubles as **BREAK**: it latches IF
  register bit `0x02` (via PB7, the LH5811 pin ON is wired to, configured
  as an interrupt input -- confirmed shared with the RTC's TP edge, see
  `IoPortController::setOnKeyLine()`) and requests the CPU's maskable
  interrupt (vector `FFF8H`); both together are what gets a running BASIC
  program to actually stop and show `BREAK IN <line>`, in addition to
  ON's own wake-from-off role.
- Shift+F12 = Off
- **Ctrl+F12 is a host-only RESET key**, mimicking the real machine's
  recessed ALL RESET pinhole switch (also not part of the matrix) — it
  re-runs `CPU::reset()` without otherwise touching RAM.

**Shift**: both host Shift keys behave identically and are read only as
modifiers (never mapped directly to a PC-1500 key) -- real PC-1500 Shift
is a tap-to-toggle key, not a hold, and holding it for a modern
keyboard's whole keypress duration confuses the ROM's key-scan. **Tab**
sends a direct, standalone PC-1500 Shift keypress for cases not covered
by the punctuation passthrough below.

**Digits and punctuation** (`charToTapActions()`, driven by `SDL_TEXTINPUT`
rather than keycodes): typing a digit or one of `!"#$%&@^<>:?;,.+-/=()*`
on the host reproduces the exact PC-1500 keypress (or Shift+key combo)
that types each one on real hardware (confirmed by Paul), queued as a
fire-and-forget sequence that runs to completion regardless of how long
the host key is held. There's a small (~0.25s) delay before the character
appears. `.`, `-`, `/`, and `=` used to have their own direct keycode
entries in `kKeyMap[]` as well, which (same root cause as the digit-row
bug below) ignored host Shift entirely -- confirmed broken on AZERTY,
where the key that types unshifted `=`/shifted `+` always produced `=`
regardless of Shift. Removed in favor of the same `SDL_TEXTINPUT` path
everything else here uses.

Because this is driven by `SDL_TEXTINPUT` (the OS's own composed text)
instead of raw keycodes, it's host-keyboard-layout-aware automatically --
e.g. on a French AZERTY host, the unshifted top-row key reproduces `&`
(what's printed on the keycap) and Shift+that key reproduces `1`, matching
the host layout rather than assuming QWERTY. (An earlier keycode-based
design didn't have this property, and was additionally broken on Windows
specifically: SDL's Windows backend hardcodes the top row's *keycode* to
always report digits regardless of the real OS layout, so AZERTY typed
"1" instead of "&" no matter what -- SDL_TEXTINPUT isn't affected by that
quirk.) Insert and Delete are the two exceptions -- not printable
characters, so they can't come from `SDL_TEXTINPUT` -- handled directly by
keycode instead, always sending Shift+Right / Shift+Left regardless of
host Shift state.

Interrupt delivery (MI/NMI/timer) is implemented, so `HLT` resumes normally
when one fires.

## Menu bar

An in-window menu bar (Dear ImGui, rendered as an SDL2 overlay — not a
native OS menu) sits above the display:

- **File > Load/Save BASIC...** — a BASIC program (filename only). Finds
  the program's bounds itself by walking the line structure from `40C5H`
  to the `FFH` end marker (see `findBasicProgramEnd()` in `main.cpp` and
  `docs/pc1500_hardware_reference.md`'s reserve-area note) — matches real
  `CLOAD`/`CSAVE` (without `M`) semantics, no address/length needed.
- **File > Load/Save BASIC Text...** — a BASIC program as a plain-text
  listing (e.g. a magazine transcription), rather than the tokenized
  binary format above. *Load* drives the ROM's own line editor via
  simulated keystrokes (so it's the real ROM tokenizing, not a
  reimplementation) — type or paste a listing into the text box, or load
  it from a file first, then click Load; any line the ROM doesn't accept
  is reported by number/content rather than silently dropped. A source
  line longer than the ROM's 79-character raw-input limit is entered
  across multiple LIST-and-append editing passes, the same technique real
  PC-1500 owners used to enter lines whose *tokenized* size exceeds what
  any single typed burst could produce — see the `loadbasictext` FIFO
  command entry below for how this works. *Save* detokenizes the current
  program into the text box using this project's own keyword table
  (`src/basic/`); its spacing is our own readable convention, not
  necessarily byte-for-byte identical to a real device's `LIST` output.
  Either box can be copied to/from the clipboard.
- **File > Load/Save Binary...** — a raw `[address, address+length)` ME0
  byte range plus a filename, matching real `CLOAD M`/`CSAVE M` semantics.
  Load has an optional "call after load" checkbox (sets the CPU's `P`
  register to the loaded address, like a hand-triggered `CALL`). Useful
  for loading `sdas`/SDCC-assembled output directly for testing.
- **Settings > Base Unit** — PC-1500 (default) or **PC-1500A**. The 'A'
  variant has 6K of built-in user RAM at `4000H`-`57FFH` instead of 2K
  (`4000H`-`47FFH`), because the 40-pin expansion port is wired
  differently (S1/S2/S3 shift to S3/S4/S5), which also shifts every
  expansion-RAM chip-select 1000H higher — see the two submenus below.
  Only takes effect on the next reset/cold-start, same as the Extension
  RAM options.
- **Settings > Extension RAM (expansion window)** — None (default) / 4K
  (**CE-151**) / 6K / 8K / 10K (full window) / **CE-155** (a real
  1982-era 8K module, split across both windows: 6K filling this window
  plus 2K isolated at exactly `3800H`-`3FFFH` in the 0000H window below —
  listed here since most of its capacity lives in this window) of
  emulated module RAM at the expansion window. The window is based at
  `4800H` on a PC-1500 (max 10K, up to `6FFFH`) or `5800H` on a PC-1500A
  (max 6K, same `6FFFH` upper bound — the menu only offers up to 6K
  there). CE-151 (4K) was the real 1982-era hardware option; the other
  plain sizes aren't real period-correct modules, but are easy to emulate
  and physically possible with modern RAM. Mutually exclusive with
  CE-163 below (a real PC-1500(A) has one expansion port, and CE-163's
  own bank-select range physically overlaps this window) — selecting a
  size (or CE-155) here turns CE-163 off, and vice versa.
- **Settings > Extension RAM (0000H)** — None (default) / 16K (synthetic;
  not a real 1982-era option, just easy headroom) / **CE-163** (a real
  1982-era module: 32K of RAM, banked two 16K halves at a time into this
  same `0000H`-`3FFFH` window). Bank selection for CE-163 on a real
  PC-1500 is a pure write-triggered address-line latch at
  `5800H`-`5FFFH` (`6800H`-`6FFFH` on a PC-1500A, per the expansion-port
  rewiring above) — writing to an *even* address there selects bank 0, an
  *odd* address selects bank 1; the byte value written doesn't matter, and
  nothing is actually stored at that address. When CE-155 (above) is
  active, this submenu shows a disabled note instead of a selectable
  item, since CE-155's own toggle lives in the expansion-window submenu —
  this window only hosts its remaining isolated 2K at `3800H`-`3FFFH`.
- **Settings > Automation Mode** — when checked, real host keyboard input
  (typing, arrow keys, F-keys, etc.) is ignored entirely; only the
  scriptable command interface below can drive the emulator. An orange
  `[AUTOMATION MODE]` notice appears in the menu bar whenever it's on, so
  it's never silently active. Meant for scripted/automated test sessions
  where an accidental real keypress landing on the window would otherwise
  corrupt whatever state is being driven over the FIFO. Toggle from the
  FIFO itself with `automation on` / `automation off`.

When "adding" RAM, it is necessary to reset the emulator (Ctrl+F12), then
press CL and execute NEW0 to update the emulator to be aware of the
additional memory.

While a menu dialog has keyboard focus, keystrokes go to the dialog's text
fields, not the emulated PC-1500 keyboard.

## Scriptable command interface

A running instance also reads commands from a named pipe, one per line,
letting an external process (or a script) drive it directly instead of
relaying everything through the GUI by hand. Query commands write their
result to a response file for the caller to read back afterward.

**Linux**: a POSIX FIFO at `/tmp/pc1500emu.cmd`, writable with plain shell
redirection; the response file is `/tmp/pc1500emu.response`.

```sh
echo 'type 10 PRINT "HELLO"' > /tmp/pc1500emu.cmd
echo 'key enter' > /tmp/pc1500emu.cmd
echo 'display' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response   # ASCII-art dump of the LCD
echo 'status' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response    # registers, flags, indicator bits
echo 'peek 4000' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response
echo 'poke 4000 ff' > /tmp/pc1500emu.cmd
echo 'dump 4000 40ff' > /tmp/pc1500emu.cmd; cat /tmp/pc1500emu.response
```

**Windows**: there's no filesystem-visible equivalent of a FIFO, so this is
a named pipe (`\\.\pipe\pc1500emu.cmd`) instead, which plain shell
redirection can't write to — use `tools/send-command.ps1`. The response
file is `%TEMP%\pc1500emu.response`.

```powershell
powershell -File tools\send-command.ps1 'type 10 PRINT "HELLO"'
powershell -File tools\send-command.ps1 'key enter'
powershell -File tools\send-command.ps1 'status'; Get-Content $env:TEMP\pc1500emu.response
```

Commands:
- `type <text>` — queues each character as a keypress (letters, digits,
  space, and a handful of symbols needing a PC-1500 Shift-tap: `" : < >`).
  Unmapped characters are skipped with a stderr warning — use `key` instead.
  Automatically taps `SML` around lowercase letters as needed so `text`'s
  own case is preserved exactly (the PC-1500 has one physical key per
  letter — case is a persistent ROM-side mode, not a separate keystroke),
  seeded from the machine's actual live SML state each time rather than a
  separately-tracked flag, so it stays correct even if `key sml` was sent
  directly between two `type` calls.
- `key <name>` — a named key with no natural printable form: `enter`, `cl`,
  `mode`, `def`, `sml`, `rcl`, `shift`, `off`, `up`/`down`/`left`/`right`,
  `f1`-`f6`, `space`. Prefix with `shift+` (e.g. `key shift+mode`) to send
  a genuine PC-1500 Shift-tap before it, same mechanism as typing a
  host-Shift symbol.
- `peek <addr>` / `poke <addr> <val>` — addresses and values in hex.
- `dump <start> <end>` — hex bytes, 16 per line, address-prefixed.
- `reset` — same as Ctrl+F12; re-runs `CPU::reset()` without touching RAM.
  Needed after `setmachine`/`setextram`/`setce163`/`setce155` below, since
  the ROM only detects the base unit's RAM shape and installed extension
  RAM at reset/cold-start, not on the fly (see "When 'adding' RAM" above).
- `setmachine <1500|1500a>` — FIFO equivalent of Settings > Base Unit.
  Affects how `setextram ext`'s window is interpreted (see that section
  above), so send this first if changing both.
- `setextram <window> <bytes>` — FIFO equivalent of the Settings >
  Extension RAM menu items. `<window>` is `ext` (the expansion window —
  `4800H`-based on a PC-1500, `5800H`-based on a PC-1500A) or `0000`;
  `<bytes>` is decimal.
- `setce163 <0|1>` — FIFO equivalent of Settings > Extension RAM (0000H) >
  CE-163 (see that section above). Mutually exclusive with `setextram`,
  `setce155`, and `setce168n` — enabling one clears the others.
- `setce155 <0|1>` — FIFO equivalent of Settings > Extension RAM (0000H) >
  CE-155 (see that section above). Mutually exclusive with `setextram`,
  `setce163`, and `setce168n`, same as `setce163` above.
- `setce168n <banks> <firstRoBank>` — a generalized, non-hardware CE-163:
  same `0000H`-`3FFFH` window, same fixed 16K-per-bank size, and same
  machine-variant-dependent bank-select trigger as CE-163 (`5800H`-`5FFFH`
  on a PC-1500, `6800H`-`6FFFH` on a PC-1500A), but with `<banks>` (total
  bank count; `0` disables the module) and `<firstRoBank>` (banks at/above
  this index simulate flash — `writeME0`/`poke` silently discard writes to them,
  though `loadbinary` can still seed their initial content) as load-time
  parameters instead of CE-163's fixed 2 banks/all-writable. Mutually
  exclusive with `setextram`, `setce163`, and `setce155`, same as those are
  with each other. FIFO-only — no GUI menu equivalent.
- `status` — CPU registers/flags and the fixed-segment indicator bits.
- `display` — the 156x7 dot matrix as ASCII art (`#`/`.`).
- `displaytext` — the ROM's own LCD text buffer (`7BB0H`-`7BFFH`, per the
  PC-2 Assembly Language manual's "DISPLAY THROUGH A BUFFER" section) read
  directly as ASCII up to its `0DH` terminator -- e.g. `ERROR 1` or
  `NEW0? :CHECK`. Prefer this over `display` for anything that's plain
  text: it's exact, where eyeballing the dot-matrix ASCII art is not.
- `savebasic <path>` / `loadbasic <path>` / `savebinary <addr> <len> <path>`
  / `loadbinary <addr> <path>` — the same functions the File menu's dialogs
  call, invokable directly without going through the GUI.
- `loadrommodule[2-4] <base hex> <requirePv 0|1> <usePuBank 0|1> <path>` —
  loads a CE-150/153/158-style plug-in ROM module into one of four
  independent slots (no suffix = slot 1) at `0x8000`-`0xBFFF`. `requirePv`
  is the PV level the CPU must have set for it to respond at all (`0`/low
  matches CE-150, `1`/high matches CE-158); the module is otherwise
  read-only ROM, matching real hardware. `unloadrommodule[2-4]` clears a
  slot.
- `loadexpansionmodule[2-4] <base hex> <requirePv 0|1> <usePuBank 0|1>
  <dataWindowBase hex> <dataWindowSize hex> <instructionAddr hex> <path>
  [sdDir]` — like `loadrommodule`, but for a module with a genuinely
  writable data window backed by a mock command processor
  (`src/bus/expansion_mock.h`), modeling boards like the PC1500-PSOC5
  expansion where that whole address range is one real, shared RAM buffer
  rather than true ROM. `path` is just the ROM bytes (a trimmed,
  base-aligned binary — not a full window-sized image with the data-window
  portion blanked out); the data window itself is separately allocated and
  initialized to `0xFF` (matching real RAM's confirmed power-up default).
  A CPU write to `instructionAddr` triggers mock command processing
  synchronously, mirroring the real board's `DoCommand()`/`WriteStatus()`
  protocol: write a command byte, poll that same address for a non-`BUSY`
  status. The optional trailing `sdDir` points the mock's SD-card commands
  (`LIST_SD_DIR`, create/open/read/write/close/remove a file, get file
  size/status/name, get free space/volume size, format) at a real host
  directory, so `SSAVE`/`SLOAD`/`SLS`/etc. can be developed and tested
  against real files instead of an embedded list — drop files in and
  `LIST_SD_DIR` sees them, `CREATE_SD_FILE`/`WRITE_TO_SD_FILE` write real
  ones there. Filenames arriving over the wire are validated against path
  traversal (rejects separators, `..`, drive letters, or anything that'd
  resolve outside `sdDir`) before touching the filesystem. **`FORMAT_SD_CARD`
  deletes every file directly inside `sdDir` — genuinely destructive,
  matching the real command's own intent — don't point it at a directory
  you care about.** Omitting `sdDir` leaves every SD command reporting
  `EXP_STATUS_ERROR` ("no card inserted"), which is itself a legitimate
  state to test. Everything else (`ROM_FROM_MCU`/`SRAM`,
  `TEST_COPY_STRING`, ...) reports `NOT_IMPLEMENTED`, same as the real
  firmware's own default case. Shares slots with `loadrommodule` (loading
  either into an already-occupied slot replaces it) —
  `unloadrommodule[2-4]` clears these too.
- `savebasictext <path>` / `loadbasictext <path>` — the text-listing
  equivalents. `loadbasictext` drives the ROM's line editor internally (see
  File menu section above) by stepping CPU/bus cycles directly rather than
  the real-time `type`/`key` queue, so it completes in well under a second
  regardless of program size; a returned `ERROR: N line(s) rejected...`
  lists which lines the ROM didn't accept. The ROM's own line editor has a
  hard 79-character raw-input limit and silently drops everything past it
  with no error shown, so a source line over 79 characters is entered
  across multiple passes instead: type up to the ROM's own limit and
  press Enter (tokenizing what's typed so far, which doesn't need to be a
  complete/valid statement), then `LIST <line#>`, jump to the end of the
  redisplayed line, and type more — repeating until the whole line is in.
  Each pass's raw typing is capped not against the line's on-screen
  length (which can already exceed 79 characters once earlier keywords
  are tokenized) but against the line's actual current *stored* size,
  read directly from bus memory; if the ROM's input buffer still silently
  drops a character or two near that estimate, the same pass is retried
  with whatever was actually accepted, picking up exactly where it left
  off, rather than trusting the estimate to be exact. Only a single
  unsplittable token (e.g. one identifier or a quoted string longer than
  79 characters on its own) still fails outright, with a clear error. A
  program whose total tokenized size exceeds the PC-1500's built-in 2K of
  RAM needs an emulated expansion module (Settings > Extension RAM) to
  load in full, same as on real hardware — see
  `docs/pc1500_hardware_reference.md`'s "BASIC line editor" section.
- `break [cycles]` — scriptable equivalent of pressing the physical ON key
  (F12) while a program is running: sets the ON-key line (which latches IF
  register bit `0x02`, confirmed shared with the RTC's TP edge -- see
  `IoPortController::setOnKeyLine()`) and requests the CPU's maskable
  interrupt (vector `FFF8H`) -- both are needed together to actually break
  a running program; either alone does nothing. If `cycles` is given,
  traces that many cycles synchronously in the same call (like
  `calltrace`) so the dispatch can actually be observed -- otherwise the
  live frame loop's own background stepping will most likely have already
  handled and returned from the interrupt by the time a separate `trace`
  call gets to look.
- `call <addr>` — sets the CPU's `P` register directly (hex address).
- `presskey <name>` / `releasekey <name>` — direct, synchronous key
  press/release (same names as `key`), bypassing the queue `type`/`key`
  use. Needed before `run`/`trace` for deterministic testing of a specific
  key's effect, since the queue only drains via the normal frame loop.
- `run <cycles>` — steps the CPU exactly `<cycles>` cycles synchronously
  (decimal), independent of the normal ~60fps frame loop. Useful for
  driving execution deterministically in a test script.
- `trace <cycles>` — like `run`, but writes one line per instruction
  executed (`PC opcode-byte A=.. X=.... Y=....`, hex) to the response file.
  For finding exactly what the CPU does over a short, specific window
  (e.g. right after a keypress) instead of only diffing memory before/after.

`type`/`key` commands queue onto the same mechanism real typing uses, so
scripted and live keyboard input interleave safely rather than racing.

**Debugging a specific keypress's dispatch.** `presskey`/`trace`/
`releasekey` sent as three separate pipe commands is tempting but unsound:
the live ~60fps frame loop keeps running in the real-world gaps between
separate commands, so a key held across a large enough `trace` budget can
get "seen" more than once by the ROM (confirmed: this let a single held
Enter both dispatch and self-exit a custom keyword within one hold — a real
bug in the debugging method, not the ROM). The commands below press, wait,
and (where relevant) trace or watch as one atomic call instead, so nothing
races them:
- `entertrace <cycles>` — presses Enter, holds it a few frames, releases,
  waits a few more idle frames, then traces `cycles` more instructions —
  all as one unbroken CPU-stepping sequence. For watching exactly what a
  keyword dispatch does immediately after Enter is pressed.
- `enterwatch <watchAddr hex> <watchLen decimal> <cycles decimal>` — same
  atomic Enter-press pattern as `entertrace`, but only logs writes to a
  specific address range instead of every instruction — for watching one
  piece of state (e.g. a status byte) change during a keyword's dispatch
  without wading through a full instruction trace.
- `typelinetrace <cycles> <text>` — types `<text>` (queued the same way as
  `type`, with a trailing Enter), then traces `cycles` more instructions
  once typing settles, atomically. The one-shot way to see what a whole
  typed command (not just a bare Enter) does on dispatch.
- `typelinewatch <watchAddr hex> <watchLen decimal> <extraCycles decimal>
  <text>` — `typelinetrace`'s watch-only counterpart, for the same reason
  `enterwatch` exists relative to `entertrace`.

A large trace or watch response can be several megabytes; read the response
file directly (e.g. with a shell tool that streams/counts lines) rather
than piping it through a language's own "read whole file into one string
variable" convenience call — that has silently truncated a multi-MB trace
to a single line in practice, with no error raised, costing real debugging
time before the mismatch between the reported line count and the file's
actual size on disk was caught.

## Disassembler

`pc1500disasm` is a standalone recursive-descent LH5801 disassembler for
ROM dumps, built alongside the emulator. Rather than a naive linear sweep,
it discovers code by following actual control flow from a set of seed
entry points, so data interleaved into code regions (string pools, jump
tables, BASIC keyword tables) renders as data, not garbage instructions.
Output is `sdas`-syntax text, directly reassemblable by
[sdaslh5801](https://github.com/pchambre/sdcc-pc1500).

```sh
./build/src/disasm/pc1500disasm --mode base ROM1.BIN -o rom1.asm          # Linux
build\src\disasm\RelWithDebInfo\pc1500disasm.exe --mode base ROM1.BIN -o rom1.asm   # Windows

# expansion module, either platform (adjust the binary path as above):
pc1500disasm --mode module --base 0xA000 CE-150.ROM -o ce150.asm

# standalone ML program, e.g. one loaded via BASIC and CALLed at 0x4268:
pc1500disasm --mode program --base 0x4268 MyProgram.bin -o myprogram.asm
```

- `--mode base` (default load address `0xC000`) seeds the reset/interrupt
  vectors and the ROM's built-in BASIC keyword table (`C01EH`).
- `--mode module` (default load address `0x8000`) scans for the `0x55`
  sentinel byte expansion modules use at each 2KB-aligned page and
  auto-detects that page's own keyword table.
- `--mode program` disassembles a standalone BASIC-`POKE`d/`CALL`ed ML
  routine: no vectors or keyword table to auto-seed from, so it just
  traverses from `--seed` (repeatable; defaults to `--base` itself if none
  given, the common case of a routine entered at its own load address).
  `--base` is required (no universal load address for a standalone routine).
- `--base 0xNNNN` overrides the default load address.
- `-o out.asm` writes to a file instead of stdout; `--annotate` adds a
  trailing `; 0xNNNN: XX XX` comment per line for human review.

Labels and operand references also carry a `; NAME -- comment` annotation
whenever the address is one of the confirmed PC-1500 memory-map/ROM
addresses in `src/disasm/known_symbols.cpp` (e.g. `E2AAH` → `IDLE`,
`764EH` → `STATUS1`, `7B0EH` → `KEYGATE`) — always on, not gated behind
`--annotate`, since it's real documentation rather than a raw byte dump.
This applies to a call/jump target (e.g. `SJP E243H`) just as much as a
direct memory reference (e.g. `LDA (764EH)`).

`--symbols-file <path>` adds your own annotations on top of that built-in
table, without a code change or rebuild -- useful for ROM routines you've
identified yourself (e.g. from the PC-2 Assembly Language manual) that
aren't in `known_symbols.cpp` yet. One entry per line:
```
# lines starting with # (and blank lines) are ignored
0xE243 KEYSCAN_WAIT scan keyboard, wait for a key
0x1234 MYROUTINE    whatever this one does
```
`<addr>` takes an optional `0x` prefix (always hex); everything after
`<name>` is the comment verbatim, so it may contain spaces. A malformed
line is skipped with a warning to stderr rather than aborting the whole
file. An entry here at the same address as a built-in one overrides it.

`--dialect sdas|tasm` (default `sdas`) chooses the output syntax for the
byte-disassembly modes above. `sdas` is this project's own house dialect,
reassemblable by `sdaslh5801`. `tasm` renders the same decoded
instructions in the [Telemark Assembler](https://www.cpcalive.com/docs/TASMMAN.HTM)
(`tasm5801.tab`) dialect real hand-written PC-1500 sources sometimes use
instead: uppercase mnemonics/registers, `$`-prefixed hex, no `.area`
wrapper (TASM has no segment/relocation concept) -- for reading/porting,
not for reassembly by this project's own toolchain.

### Converting a TASM source to sdas

`--mode convert` goes the other direction: rewrites a hand-written TASM
(`tasm5801.tab`) `.asm` file to this project's sdas dialect, reassemblable
by `sdaslh5801`.

```sh
pc1500disasm --mode convert memtest.asm -o memtest.sdas.asm
```

Confirmed against two independent real TASM PC-1500 sources: a
hand-written memory-test program, and a much larger ROM disassembly
([github.com/Jeff-Birt/Sharp_CE-158](https://github.com/Jeff-Birt/Sharp_CE-158)).
Handles `.EQU`/`.ORG`/`.DB`/`.BYTE`/`.DW`/`.WORD`/`.TEXT`/`.ASCII`, `$`-hex
literals, uppercase mnemonics/registers, and three TASM mnemonics that
alias LH5801 opcodes under Z80/8080-familiar names with no sdas
equivalent -- `CALL`/`RET`/`SCF` for `SJP`/`RTN`/`SEC` -- these get a real
rename, not just a case fold. A trailing `.END` is dropped rather than
translated: this project's own disassembler never emits one, and a real
`sdaslh5801` build actively rejects a trailing `.end`.

Deliberately out of scope -- reported as errors rather than silently
mis-converted, since these need a real preprocessor/macro-expander, not a
syntax-level rewrite: `#include`/`#define`/`#ifdef` preprocessor
directives, `.EXPORT` (cross-module linking), and `MACRO`/`ENDM` blocks.
A second `.ORG` in one file is a warning, not an error (only the first
gets a synthesized `.area CODE (ABS)` wrapper) -- multi-segment TASM
sources aren't fully supported. An unrecognized mnemonic/directive token
is also a warning: passed through unchanged, best-effort, so check the
output actually assembles. See `src/disasm/tasm_convert.h`'s own comment
for the full rationale.

### Editing disassembly output

No dedicated LH5801 IDE exists anywhere (checked). `tools/vscode-lh5801-asm/`
is a small local VS Code extension providing syntax highlighting for the
`sdas` dialect this disassembler emits and `sdaslh5801` accepts, plus a
**"LH5801: Disassemble to ASM"** command that runs `pc1500disasm` for you:
right-click a `.ROM`/`.BIN` file in the Explorer (or run the command from
the Command Palette, which then prompts you to pick one), choose a mode
(base/module/program), confirm the load address, and it writes and opens
the resulting `.asm`. Set `lh5801.disasmCommand` in `settings.json` first
(defaults to this repo's own Windows build output,
`build/src/disasm/RelWithDebInfo/pc1500disasm.exe` — adjust for Linux/Mac).
Install the extension by copying `tools/vscode-lh5801-asm/` into
`%USERPROFILE%\.vscode\extensions\` (Windows) or `~/.vscode/extensions/`
(Linux/Mac), then reload the window.

**"LH5801: Convert TASM to SDAS"** — right-click a `.asm`/`.s` file (or run
from the Command Palette against the active editor) to run `pc1500disasm
--mode convert` (see above) and write/open the result via a Save dialog
(defaulting to `<name>.sdas.asm` beside the source). Uses the same
`lh5801.disasmCommand` setting as "Disassemble to ASM" — no separate
setting needed. A conversion error (an unsupported construct) shows every
reported line, since each is a distinct thing to fix; a warning (e.g. an
unrecognized token, or a second `.ORG`) still opens the output, since it
was written regardless.

The extension also has three commands that take you from source to a
running (or debugged) program without leaving the editor:

- **"LH5801: Assemble to BIN"** — right-click a `.asm`/`.s` file (or run
  from the Command Palette against the active editor) to run
  `sdaslh5801` → `sdld` → `makebin` and produce a flat, loadable
  `<name>.bin` beside the source (plus `<name>.lst`/`.ihx`/`.map`/etc.,
  left in place rather than cleaned up, in case a link error needs
  investigating). No load-address prompt is needed: the address comes
  straight from the source file's own `.area ... (ABS)` / `.org`
  (matching this project's convention — see `ce150.asm`-style output),
  read back out of the linked `.ihx`'s first data record. Requires
  `lh5801.sdasCommand`, `lh5801.linkCommand`, and `lh5801.makebinCommand`
  set in `settings.json` — a plain native path on any platform (Windows:
  an MSVC or MSYS2 build, e.g. `sdcc/bin_vc/sdaslh5801.exe`; Linux/Mac:
  e.g. `sdcc/bin/sdaslh5801`), or `wsl <path>` on Windows for a WSL-only
  build (see each setting's own description for the exact forms).
- **"LH5801: Build C to BIN"** — the same idea for a `.c` file, running
  sdcc-pc1500's own `build-lh5801.sh` (prompts once for the load
  address, since a C program's base isn't self-evident the way a
  hand-assembled/disassembled `.asm`'s `.org` is). Requires
  `lh5801.buildCCommand` — since it's a bash script, that's always the
  `wsl ...` form on Windows, or a plain path (or `bash <path>`) on
  Linux/Mac.
- **"LH5801: Run in pc1500emu"** and **"LH5801: Debug in pc1500emu"** —
  available on `.asm`/`.s`/`.c` files (assembling/building first) or an
  already-built `.bin`/`.rom`. Prompts for how to load it (as a plain
  application via `loadbinary`, or as a CE-150/153/158-style plug-in ROM
  via `loadrommodule`, picking one of four independent slots), then
  either drives an already-running (or freshly-launched, via
  `lh5801.emulatorPath`/`lh5801.romPath`) `pc1500emu` directly over the
  command pipe and `call`s the entry point, or starts an `lh5801` debug
  session the same way F5 would (see Debugger below) — you're asked
  whether to attach to a running instance or launch a fresh one.
  A *freshly-launched* instance (never one you're attaching to) also
  picks up `lh5801.extRamExtBytes`/`lh5801.extRam0000Bytes`/
  `lh5801.isPC1500A` (extension RAM and base unit — many real programs,
  e.g. anything using `printf()`, need more than the base 2KB) and
  `lh5801.preloadRomModules` (auxiliary ROM modules, e.g. a CE-150/158,
  loaded before the main program). Any of the RAM/variant settings being
  non-default forces that launch to boot cold (`--no-state`) — they're
  only detected by the ROM on a fresh boot, not a state restore.

Opening this repo in VS Code also picks up:
- `.vscode/settings.json` — associates `*.asm`/`*.s` with the extension's
  grammar (scoped to this project only), and holds `lh5801.disasmCommand`,
  `lh5801.sdasCommand` (below), and the `lh5801.linkCommand`/
  `lh5801.makebinCommand`/`lh5801.buildCCommand`/`lh5801.emulatorPath`/
  `lh5801.romPath`/`lh5801.extRamExtBytes`/`lh5801.extRam0000Bytes`/
  `lh5801.isPC1500A`/`lh5801.preloadRomModules` settings the commands
  above use.
- `.vscode/tasks.json` — a build task (`Ctrl+Shift+B`) that runs
  `sdaslh5801` on the active file and reports errors in the Problems panel.
  Fill in `lh5801.sdasCommand` in `settings.json` with your own built
  `sdaslh5801` path first (see the setting's own comment for the WSL vs.
  MSYS2 form) — it isn't built by default.
- `.vscode/extensions.json` — recommends Microsoft's official
  [Hex Editor](https://marketplace.visualstudio.com/items?itemName=ms-vscode.hexeditor)
  extension for viewing a ROM's raw bytes (hex grid + decoded text) next to
  its disassembly: open the `.BIN`/`.ROM` file, "Open With" → Hex Editor,
  then drag its tab into a split pane alongside the generated `.asm`.

### Debugger

`pc1500debugadapter` (built alongside the emulator, `src/debugadapter/`) is
a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
server that lets VS Code's native debugging UI — breakpoints, step,
call stack, variables — drive a running `pc1500emu` instance, via the same
scriptable command pipe described above (`setbreakpoints`/`continue`/
`pause`/`debugstep`/`debugstatus`).

`tools/vscode-lh5801-asm/`'s `contributes.debuggers` entry wires this up as
debug type `lh5801`; `.vscode/launch.json` has two ready-made
configurations:

- **"LH5801: Attach to running pc1500emu"** — launch `pc1500emu` yourself
  first, then F5 to load a program into it and start debugging.
- **"LH5801: Launch pc1500emu and debug"** — starts `pc1500emu` itself
  (fill in `romPath` for your machine first).

These are templates for a specific, already-assembled `program`/`listing`
pair — fill in the paths and addresses by hand. The extension's own
**"LH5801: Debug in pc1500emu"** command (see Editing disassembly output
above) does the same thing but builds this configuration for you from a
`.asm`/`.c`/`.bin` file directly, including the assemble/build and
load-address steps, and is the easier path for day-to-day use.

Both load a program (`program`/`loadAddress`/`loadMode` — raw binary via
`loadbinary`, or a CE-150-style plug-in ROM via `loadrommodule`), set the
entry point (`entry`), and, if a `listing` (`sdaslh5801 -l` output) is
given, resolve source-line breakpoints and stack-frame source locations
against it. Without a `listing`, debugging still works at the instruction
level (breakpoints by address, stepping, registers, memory) — VS Code just
won't be able to show a source line for the current position.

When `emulatorPath` is given (a fresh launch, not an attach), two more
launch properties apply: `emulatorArgs` (extra CLI args, overriding the
bare `romPath` positional arg — this is how "LH5801: Debug in pc1500emu"
passes a generated `--conf`/`--no-state` for extension RAM, see above)
and `preloadRomModules` (auxiliary ROM modules loaded before `program`
itself). Both are usually auto-populated by the extension command, not
meant to be hand-written into `launch.json`.

Known limitations:

- **Only one stack frame is ever reported.** The LH5801 has no easy
  hardware call-stack walk (return addresses just live on the CPU's own
  stack register `S`), so "Call Stack" always shows a single `PC` frame,
  not a full unwind.
- **Stop detection is poll-based, not a push notification.** The command
  pipe is a plain request/response channel with no way for the emulator to
  initiate a message, so after `continue` the adapter polls `debugstatus`
  every ~75ms until it reports stopped. A `pause` typically takes effect
  within that same window, not instantly.
- **Full end-to-end use (F5 → hit a breakpoint → see the source line
  highlighted) needs a built `sdaslh5801`** to produce the `.lst` listing
  files this depends on for source-line mapping — not built on this
  machine as of this writing. The adapter itself is fully working and
  tested against hand-built binary+listing fixtures (see
  `tests/listing_file_test.cpp`, `tests/dap_protocol_test.cpp`, and the
  FIFO-driven breakpoint tests exercised directly against a live
  `pc1500emu`); what's untested is specifically real `sdaslh5801 -l`
  output.

## Layout

- `src/cpu/` — LH5801 CPU core
- `src/bus/` — memory map, bus I/O dispatch
- `src/keyboard/` — keyboard matrix emulation
- `src/lcd/` — dot-matrix LCD controller emulation
- `src/host/` — host-side glue (windowing, input, main loop, menu bar)
- `src/basic/` — BASIC keyword tokenizer/detokenizer and keystroke-driven
  program load/save
- `src/disasm/` — LH5801 ROM disassembler (`pc1500disasm`)
- `src/debugadapter/` — DAP server bridging VS Code's debugging UI to a
  running `pc1500emu` (`pc1500debugadapter`, see the Debugger section above)
- `tools/vscode-lh5801-asm/` — local VS Code extension for editing
  disassembly output and debugging (see the Disassembler/Debugger
  sections above)
- `third_party/imgui/` — vendored Dear ImGui (menu bar UI), MIT licensed
- `tests/` — unit tests
- `docs/` — technical reference notes (ISA, hardware) backing the implementation

## License

Licensed under the Apache License, Version 2.0 -- see `LICENSE`.
