// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
//
// File-level framing (magic + format version) around CPU::saveState/
// loadState and Bus::saveState/loadState -- see each's own comment for
// exactly what's captured. Deliberately includes full CPU register/flag/
// interrupt-latch state (format version 2; version 1 didn't; version 3
// widened Bus::saveState/loadState from two ROM module slots to four;
// backing store; version 5 added the PC-1500/PC-1500A machine-variant
// flag and the CE-155 module's enabled flag; version 6 reordered
// Bus::saveState/loadState to write the small RAM-config scalars first,
// before the large me0_/ce163Ram_/romModules blobs, so loadState can
// reject a state whose saved config doesn't match the currently
// configured hardware without needing to read past them first -- see
// Bus::loadState's own comment for why that matters: raw memory contents
// saved under one RAM configuration are meaningless -- not just for the
// reserve-key area, but in general -- loaded into a different one, the
// same way real PC-1500 RAM (short of a battery-backed module) doesn't
// survive a hardware reconfiguration either; version 7 added the CE-168N
// module's enabled flag, active bank, bank count, first-read-only-bank,
// and variable-size backing store) -- restoring a session is meant to
// resume exactly where OFF left the machine, the same way real hardware's
// OFF/ON cycle just halts and wakes the CPU in place rather than
// resetting it, so the caller must NOT call cpu.reset() after a
// successful loadStateFile.
#pragma once

#include <cstdint>
#include <string>

#include "bus.h"
#include "lh5801.h"

namespace pc1500host {

inline constexpr char kStateFileMagic[4] = {'P', 'C', '1', 'S'};
inline constexpr uint16_t kStateFileVersion = 7;

// Writes an 8-byte header (4-byte magic, u16 version, 2 reserved bytes),
// then cpu.saveState(), then bus.saveState(). Returns false with *error
// set if `path` can't be opened for writing.
bool saveStateFile(const lh5801::CPU& cpu, const pc1500::Bus& bus, const std::string& path,
                    std::string* error);

// Validates the header (magic must match; formatVersion must equal
// kStateFileVersion exactly -- this is early-stage software with no
// installed base to keep old state files compatible with, so a version
// mismatch in either direction is simply rejected rather than partially
// misread) before calling cpu.loadState()/bus.loadState(). Returns false
// -- with *error set, and cpu/bus possibly partially modified, so callers
// should treat any false return as fatal to this restore attempt -- if
// the file is missing/unreadable, too short, has the wrong magic, or a
// different version. On success, the caller must NOT follow up with
// cpu.reset() -- see the file header comment. `configMismatch` and
// `savedConfig`, if non-null, are forwarded from Bus::loadState -- see its
// own comment -- and are only meaningfully set when this returns false
// specifically because of a config mismatch (as opposed to a missing/bad-
// magic/wrong-version/truncated file, none of which populate them).
bool loadStateFile(lh5801::CPU& cpu, pc1500::Bus& bus, const std::string& path, std::string* error,
                    bool* configMismatch = nullptr, pc1500::Bus::SavedConfig* savedConfig = nullptr);

}  // namespace pc1500host
