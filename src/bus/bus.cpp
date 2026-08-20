// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "bus.h"

#include "serialize_io.h"

namespace {
inline std::tm portable_localtime(std::time_t t) {
  std::tm result{};
#if defined(_WIN32)
  localtime_s(&result, &t);
#else
  localtime_r(&t, &result);
#endif
  return result;
}
}  // namespace

namespace pc1500 {

namespace {
uint8_t toBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

// Debounce window for Upd1990ac::consumeRisingEdge(), anchored to
// Upd1990ac::tp()'s (OPB's) own most recent read -- see consumeRisingEdge()'s
// own comment for why this specific anchor (as opposed to a single window
// shared by both reads, or a fixed time grid -- both tried first and made
// things worse). 100us: measured live (testWaitPollTrace, direct
// instrumentation of Upd1990ac's own internal debounce timestamp, not
// inferred from instruction counts -- an initial ~6us estimate from
// counting traced instructions turned out to only be measuring the cost
// of the *bii+bzr pair itself*, not the actual gap to IF's read) that
// WAIT/BEEP's poll loop's OPB read and its IF fallback read (LE89C-LE8BC
// in _bisect/rom1.asm, F00FH bit 5, then a vmj + the E451 helper's own
// bii+rtn, F00BH bit 1) are ~61us apart at 1.3MHz -- so 100us comfortably
// covers that gap with margin (guaranteeing the two always agree) while
// staying well below TP's own half-period even at its fastest configured
// rate (2048Hz, ~244us -- the 64Hz case this code path actually exercises
// has an ~7.8ms half-period, wider still), so no genuine edge can hide
// between polls the way one could under the old once-per-rendered-frame
// model, and a tick caught via IF instead of OPB (BREAK, or the rare read
// with no recent OPB check) is never delayed by more than 100us of real
// time -- still negligible against a millisecond-scale period.
constexpr double kTpResyncDebounceSeconds = 0.0001;
}  // namespace

void Upd1990ac::setControlPins(bool dataIn, bool stb, bool clk, bool c0, bool c1, bool c2) {
  dataIn_ = dataIn;

  if (stb && !prevStb_) latchCommand(c0, c1, c2);
  prevStb_ = stb;

  if (clk && !prevClk_ && (mode_ == Mode::RegisterShift || mode_ == Mode::TimeSet)) {
    shiftRegister_ = (shiftRegister_ >> 1) |
                      (static_cast<uint64_t>(dataIn_ ? 1 : 0) << 39);
  }
  prevClk_ = clk;
}

void Upd1990ac::latchCommand(bool c0, bool c1, bool c2) {
  int sel = (c1 ? 2 : 0) | (c0 ? 1 : 0);
  if (!c2) {
    Mode oldMode = mode_;
    switch (sel) {
      case 0: mode_ = Mode::RegisterHold; break;
      case 1: mode_ = Mode::RegisterShift; break;
      case 2: mode_ = Mode::TimeSet; break;
      case 3:
        mode_ = Mode::TimeRead;
        // "PS" snapshot per the datasheet's block diagram: Time Read
        // copies the live running clock into the shift register: it's
        // Register Shift (or continuing to sit in Time Read, which the
        // datasheet doesn't distinguish for this purpose) that then
        // actually walks it out over DATA OUT via CLK pulses.
        shiftRegister_ = liveTimeAsBcd40();
        break;
      default: break;
    }
    // Time Set & Counter Hold pauses the live clock while new data is
    // shifted in; committing on exit (rather than bit-by-bit as each CLK
    // arrives) is externally indistinguishable to anything that only reads
    // the result afterward, and far simpler.
    if (oldMode == Mode::TimeSet && mode_ != Mode::TimeSet) commitShiftRegisterToTime();
    // Any Group 0 (register-mode) command means WAIT/BEEP is done with TP
    // -- confirmed via disassembly: both LE8B4 (poll loop abort) and LE8C3
    // (poll loop's own successful completion, after the full 16-bit
    // countdown reaches zero) issue this exact "ldi a,0 / vmj 0x5A"
    // sequence (Group 0, sel=0/RegisterHold) as their own cleanup, and
    // nothing else in the ROM ever issues a Group 1 (TP-rate-select)
    // command at all -- WAIT/BEEP is TP's only caller. Without this,
    // tpConfigured_ stayed permanently latched true after the *first*
    // WAIT/BEEP ever run for the life of the process: TP kept oscillating
    // in the background forever, and consumeRisingEdge() (called by any
    // later, unrelated IF read -- e.g. the idle READY-prompt loop's own
    // BREAK check, KEYSCAN_WAIT/LE269, or the generic statement-boundary
    // break-check LC42A) kept discovering and reporting genuine RTC ticks
    // as if they were BREAK presses, since both share this same bit with
    // no way to tell the source apart -- confirmed live: the screen
    // visibly cleared periodically at the idle prompt, long after any
    // WAIT statement had actually finished running. Resetting here is
    // symmetric with the *other* end of this same gate (see
    // consumeRisingEdge()'s "Returns false unconditionally until..."
    // comment for why TP shouldn't mean anything before WAIT/BEEP
    // configures it either).
    tpConfigured_ = false;
    tpLevel_ = false;
    tpEdgePending_ = false;
    everOpbSynced_ = false;
  } else {
    switch (sel) {
      case 0: tpRateHz_ = 64; tpConfigured_ = true; break;
      case 1: tpRateHz_ = 256; tpConfigured_ = true; break;
      case 2: tpRateHz_ = 2048; tpConfigured_ = true; break;
      default: break;  // TEST MODE -- not modeled, treated as a no-op
    }
    // Every (re-)configure resets TP's phase to a fresh, known boundary --
    // matching the observable effect of the datasheet's own TP-rate-select
    // command (which necessarily also has some real reset effect on the
    // divider chain), and giving WAIT/BEEP's own poll loop a clean edge to
    // synchronize against right after each configure, same as it does on
    // real hardware.
    if (tpConfigured_) {
      tpEpoch_ = now();
      tpLevel_ = false;
      tpEdgePending_ = false;
      // Force consumeRisingEdge() to do its own genuine fresh sample on
      // the next read rather than trusting a now-stale pre-(re-)configure
      // OPB timestamp -- see its own comment.
      everOpbSynced_ = false;
    }
  }
}

bool Upd1990ac::dataOut() const {
  if (mode_ == Mode::RegisterShift || mode_ == Mode::TimeSet) return (shiftRegister_ & 1) != 0;
  // Register Hold / Time Read: DATA OUT is a fixed status waveform (1 Hz /
  // 0.5 Hz respectively per the datasheet's command table), not register
  // content -- computed on demand rather than tracked as separate phase
  // state, since nothing in the real protocol depends on catching a
  // specific edge of it.
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  if (mode_ == Mode::TimeRead) return ((ms / 1000) % 2) != 0;  // 0.5 Hz
  return ((ms / 500) % 2) != 0;                                // 1 Hz
}

void Upd1990ac::syncTp() const {
  if (!tpConfigured_) return;
  double elapsed = std::chrono::duration<double>(now() - tpEpoch_).count();
  double halfPeriod = 0.5 / tpRateHz_;
  // Parity of the number of half-periods elapsed since tpEpoch_ -- a pure
  // function of real elapsed time, recomputed fresh on every call rather
  // than incrementally accumulated, so it's exact regardless of how long
  // it's been since the last sync (no batching, no possibility of a
  // transition happening "between" two syncs without one of them landing
  // exactly on it -- see this class's own comment for why that mattered).
  long long intervals = static_cast<long long>(elapsed / halfPeriod);
  bool newLevel = (intervals % 2) != 0;
  if (newLevel && !tpLevel_) {
    tpEdgePending_ = true;
    debugTotalToggleCount_++;
  } else if (newLevel != tpLevel_) {
    debugTotalToggleCount_++;
  }
  tpLevel_ = newLevel;
}

bool Upd1990ac::tp() const {
  // Always a fresh, undebounced resample -- OPB (F00FH bit 5) is the
  // "primary" signal here; see consumeRisingEdge()'s own comment for why
  // IF's read (F00BH bit 1), when it happens shortly afterward, instead
  // trusts what THIS just established rather than independently
  // re-checking. Combines the live level with any not-yet-consumed
  // pending edge, and consumes it, in one atomic step -- deliberately
  // *not* split into two separately-callable pieces (an earlier version
  // had IoPortController::read()'s OPB case call consumeRisingEdge() and
  // tp() as two separate statements, in the wrong order, which silently
  // defeated this whole debounce scheme: the edge-check ran first,
  // against the *previous* read's OPB timestamp, before this call had a
  // chance to establish a fresh one).
  syncTp();
  lastOpbSyncTime_ = now();
  everOpbSynced_ = true;
  bool edge = tpEdgePending_;
  tpEdgePending_ = false;
  return tpLevel_ || edge;
}

bool Upd1990ac::consumeRisingEdge() const {
  // Debounced relative to OPB's own most recent read -- see
  // kTpResyncDebounceSeconds's own comment for why a debounce at all, and
  // why *this* specific anchor. WAIT/BEEP's poll loop (LE89C-LE8BC in
  // _bisect/rom1.asm) always reads OPB (tp(), above) first, falling back
  // to this only a handful of instructions later -- and the two must
  // agree, or the ROM's own branch structure reads "IF set while OPB was
  // low" as BREAK when it's really just this read seeing an edge OPB's
  // read a moment earlier genuinely hadn't reached yet. A single debounce
  // window covering *both* tp() and this was tried first and made things
  // *worse*: measured live, the actual OPB-to-IF gap (~61us, via a vmj
  // plus the E451 helper's own bii+rtn) is *longer* than one full
  // OPB-to-OPB loop iteration (~15us) -- so no single shared window can
  // both be wide enough to let IF reuse OPB's answer and narrow enough
  // for OPB's own next read to still count as "new"; picking one tried
  // first (a fixed grid wide enough to cover the OPB-to-IF gap) just
  // debounced OPB's own reads against each other too, leaving OPB just as
  // capable of going stale as IF was, and -- being fully deterministic --
  // failing the same way on every single tick rather than a rare few.
  // Exempting OPB entirely (tp() always resamples) and debouncing only
  // this method against OPB's own
  // timestamp keeps OPB authoritative while still giving this a fixed
  // reference point to agree with. Falls back to an independent fresh
  // resample when no recent OPB read exists at all -- BREAK's own
  // detection path (KEYSCAN_WAIT/LE269, C400/LC42A) never reads OPB, and
  // neither does bus_test.cpp's standalone testRtcTpRateSelectAndIfBitLatch.
  if (!everOpbSynced_ ||
      std::chrono::duration<double>(now() - lastOpbSyncTime_).count() >=
          kTpResyncDebounceSeconds) {
    syncTp();
  }
  bool edge = tpEdgePending_;
  tpEdgePending_ = false;
  return edge;
}

uint64_t Upd1990ac::liveTimeAsBcd40() const {
  auto now = std::chrono::system_clock::now() + timeOffset_;
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmv = portable_localtime(t);

  uint8_t month = static_cast<uint8_t>(tmv.tm_mon + 1);  // 1-12, binary/hex -- not BCD
  uint8_t dow = static_cast<uint8_t>(tmv.tm_wday);        // 0-6, Sunday=0
  uint8_t day = toBcd(tmv.tm_mday);
  uint8_t hour = toBcd(tmv.tm_hour);
  uint8_t minute = toBcd(tmv.tm_min);
  uint8_t second = toBcd(tmv.tm_sec);

  uint64_t reg = 0;
  reg |= static_cast<uint64_t>(second & 0x0F) << 0;
  reg |= static_cast<uint64_t>((second >> 4) & 0x0F) << 4;
  reg |= static_cast<uint64_t>(minute & 0x0F) << 8;
  reg |= static_cast<uint64_t>((minute >> 4) & 0x0F) << 12;
  reg |= static_cast<uint64_t>(hour & 0x0F) << 16;
  reg |= static_cast<uint64_t>((hour >> 4) & 0x0F) << 20;
  reg |= static_cast<uint64_t>(day & 0x0F) << 24;
  reg |= static_cast<uint64_t>((day >> 4) & 0x0F) << 28;
  reg |= static_cast<uint64_t>(dow & 0x0F) << 32;
  reg |= static_cast<uint64_t>(month & 0x0F) << 36;
  return reg;
}

void Upd1990ac::commitShiftRegisterToTime() {
  auto nibble = [this](int shift) { return static_cast<int>((shiftRegister_ >> shift) & 0x0F); };
  int second = nibble(4) * 10 + nibble(0);
  int minute = nibble(12) * 10 + nibble(8);
  int hour = nibble(20) * 10 + nibble(16);
  int day = nibble(28) * 10 + nibble(24);
  int month = nibble(36);  // 1-12, binary/hex

  auto now = std::chrono::system_clock::now();
  std::time_t nowT = std::chrono::system_clock::to_time_t(now);
  std::tm tmv = portable_localtime(nowT);  // seeds tm_year -- the chip has no year field at all
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = minute;
  tmv.tm_sec = second;
  std::time_t newT = std::mktime(&tmv);
  auto newTimePoint = std::chrono::system_clock::from_time_t(newT);
  timeOffset_ = std::chrono::duration_cast<std::chrono::seconds>(newTimePoint - now);
}

uint8_t IoPortController::read(uint8_t reg) const {
  switch (reg & 0x0F) {
    case 0x05:
      if_ &= static_cast<uint8_t>(~kRdFlagBit);
      return serialRxByte_;
    case 0x07: return f_;
    case 0x08: return opc_;
    case 0x09: return g_;
    case 0x0A: return msk_;
    case 0x0B: {
      // BREAK's own detection path (KEYSCAN_WAIT/LE269, C400/LC42A) reads
      // IF directly, never through OPB (F00FH) -- so this needs its own
      // lazy RTC resync, independent of OPB's read (see its case above),
      // confirmed necessary by testRtcTpRateSelectAndIfBitLatch (bus_test.cpp),
      // which reads IF alone, well after a rising edge, with no OPB read
      // ever happening first.
      //
      // Deliberately does NOT share state with OPB's read beyond both
      // drawing on the same rtc_ -- earlier versions had OPB's read *also*
      // latch a caught edge into if_ (so a subsequent IF check would see
      // it too), which looked reasonable but actually caused the exact
      // bug it was trying to prevent: TP's high phase lasts ~7.8ms (64Hz),
      // thousands of CPU cycles, so WAIT's poll loop (LE89C-LE8BC in
      // _bisect/rom1.asm) re-checks OPB *many* times while still
      // genuinely, correctly mid-tick -- and every one of those re-checks
      // used to re-discover the *same* already-actioned edge sitting in
      // if_ (nothing had cleared it yet), which LE89C's own busy-spin
      // structure has no path back from for "OPB still high, IF also
      // still set" -- it falls straight to the abort path (LE8B4) on the
      // very next iteration after every single decrement, not just the
      // first. Since IF only ever needs to answer "did a *rising edge*
      // happen" (not "is TP currently high", which is OPB's own job), and
      // this case already independently, correctly answers that via its
      // own consumeRisingEdge() call, OPB's read has nothing useful left
      // to contribute here.
      if (rtc_.consumeRisingEdge()) if_ |= kTpFlagBit;
      // Deliberately does NOT clear kTpFlagBit here (unlike kRdFlagBit
      // above, which genuinely is a per-read-consumed latch) -- an
      // earlier version did, and it broke BREAK: the MI interrupt handler
      // itself (LE171) reads this exact register to test a *different*
      // bit (bit 0, LE17E's "bii #(0xF00B),0x01") as part of its own,
      // unrelated dispatch logic -- and since read() returns/clears the
      // whole byte regardless of which bits the caller's own bii
      // instruction actually tests, that read was silently eating
      // kTpFlagBit as a side effect, on literally every BREAK-triggered
      // MI dispatch, before the interpreter's own statement-boundary
      // break-check (LC42A) ever got a chance to see it -- confirmed live
      // via cycle-accurate tracing (a held BREAK during a running FOR/NEXT
      // loop had zero effect; the ~microsecond-long window right after
      // OPB's own read consumed a tick, before this register was next
      // read for something unrelated, was the only time BREAK could ever
      // slip through). Safe to leave set until an explicit write clears
      // it (LC4C6/LC4ED's "ani #(0xF00B),0xFD" -- the only such write
      // anywhere in the ROM, and the same real hardware convention this
      // whole register already follows) instead of clearing on every
      // read: OPB's own read (tp(), case 0x0F above) already consumes
      // rtc_'s pending edge before this line ever runs during WAIT's own
      // poll loop (which always checks OPB first), so this line is
      // already a no-op for RTC-caused activity in the only place that
      // used to depend on it self-clearing.
      return if_;
    }
    case 0x0C: return dda_;
    case 0x0D: return ddb_;
    case 0x0E: return opa_;
    case 0x0F: {
      // Bits 5/6 are hardwired to the RTC's TP/DATA OUT pins (see rtc_ and
      // docs/pc1500_hardware_reference.md), read unconditionally like PB7
      // regardless of DDB -- these aren't general-purpose I/O on a stock
      // PC-1500. Bit 3 (PB3) is hardwired to VCC on export units (GND on
      // domestic ones) -- previously documented as "no logical function"
      // since nothing writes it, until tracing the SML-key regression
      // (2026-08-06) found the ROM's keyboard dispatch at E3F6H *reads*
      // PB3 to decide whether to run its real Small-toggle code (E40CH,
      // EAI #08H against 764EH) or fall into an inert cleanup path. With
      // PB3 left at its opb_ default of 0 (unset), every SML press took
      // the inert path and the Small status bit could never be set -- SML
      // silently never worked on this export-ROM build. Forcing this bit
      // high (matching VCC) is what real export hardware's dispatch code
      // is actually reading.
      // Deliberately does NOT touch if_ (IF, F00BH) at all -- see case
      // 0x0B's own comment for why cross-latching OPB's read into IF
      // turned out to be the actual bug, not the fix it looks like at
      // first glance. rtc_.tp() itself combines the live level with any
      // not-yet-consumed pending edge (see its own comment) -- must be
      // called as this one atomic step, not split into a separate
      // edge-check-then-level-check pair (that ordering bug is exactly
      // what silently defeated the debounce scheme the first time this
      // was written).
      bool tpBit = rtc_.tp();
      uint8_t v = static_cast<uint8_t>(opb_ & 0x1F);
      v = static_cast<uint8_t>(v | 0x08);
      v = static_cast<uint8_t>(v | (tpBit ? 0x20 : 0x00));
      v = static_cast<uint8_t>(v | (rtc_.dataOut() ? 0x40 : 0x00));
      v = static_cast<uint8_t>(v | (onKeyLine_ ? 0x80 : 0x00));
      return v;
    }
    case 0x06: return 0xFF;  // write-only trigger register, per Service Manual register table
    default: return 0xFF;  // divider reset (0100): not modeled
  }
}

void IoPortController::write(uint8_t reg, uint8_t value) {
  switch (reg & 0x0F) {
    case 0x06:
      serialTxByte_ = value;
      if_ &= static_cast<uint8_t>(~kTdFlagBit);
      txCyclesRemaining_ = 11 * transmitClockDivisor();
      break;
    case 0x07: f_ = value; break;
    case 0x08:
      opc_ = value;
      debugOpcWriteCount_++;
      debugLastOpcValue_ = value;
      debugOpcWriteHistory_.push_back(value);
      buzzerOn_.store((value & 0x40) != 0, std::memory_order_relaxed);
      // PC0-PC5 -- see rtc_'s class comment for the confirmed pin mapping
      // (PC0=DATA IN, PC1=STB, PC2=CLK, PC3-5=C0-C2).
      rtc_.setControlPins((value & 0x01) != 0, (value & 0x02) != 0, (value & 0x04) != 0,
                           (value & 0x08) != 0, (value & 0x10) != 0, (value & 0x20) != 0);
      break;
    case 0x09: g_ = value; break;
    case 0x0A: msk_ = value; break;
    case 0x0B: if_ = value; break;
    case 0x0C: dda_ = value; break;
    case 0x0D: ddb_ = value; break;
    case 0x0E: opa_ = value; break;
    case 0x0F: opb_ = value; break;
    case 0x05: break;  // U is read-only ("write: n/a" per Service Manual register table)
    default: break;  // divider reset (0100): not modeled
  }
}

void IoPortController::advanceCycles(int cycles) {
  if (txCyclesRemaining_ <= 0) return;
  txCyclesRemaining_ -= cycles;
  if (txCyclesRemaining_ <= 0) {
    txCyclesRemaining_ = 0;
    if_ |= kTdFlagBit;
  }
}

uint8_t IoPortController::opaOutput() const {
  // Bits set to output (DDA=1) drive opa_'s value; bits left as input
  // (DDA=0) read back as 1 (undriven/pulled-up). Real firmware (and our
  // own keyscan probe, docs/pc1500_keyscan_probe.md) always sets DDA=FFH
  // before using PA as keyboard strobe outputs.
  return static_cast<uint8_t>((opa_ & dda_) | static_cast<uint8_t>(~dda_));
}

uint8_t Bus::readME0(uint16_t addr) {
  lastAccessedSpace_ = MemorySpace::ME0;
  if (addr >= 0x8000 && addr <= 0xBFFF) {
    uint8_t v;
    // Live data-window bytes take priority over static ROM bytes -- a
    // module with hasDataWindow can genuinely be written to (see
    // writeME0), unlike a plain read-only loadrommodule-style module.
    for (const RomModule& m : romModules_) {
      if (m.tryReadWindow(addr, pv_, v)) return v;
    }
    for (const RomModule& m : romModules_) {
      if (m.tryRead(addr, pv_, pu_, v)) return v;
    }
    return 0xFF;  // empty socket, or a module present but not selected by the current PV level
  }
  // CE-163: reads its own separate 32K backing store directly, bypassing
  // me0_ entirely, so the bank not currently selected keeps its contents
  // (see Bus::setCe163Enabled's own comment for why this can't just be a
  // size gate into me0_ like the other two extension-RAM windows).
  if (ce163Enabled_ && addr <= 0x3FFF) {
    return ce163Ram_[static_cast<size_t>(ce163Bank_) * 0x4000 + addr];
  }
  // CE-168N: same idea as CE-163 above, generalized to a parametrized bank
  // count -- see Bus::setCe168nEnabled's own comment.
  if (ce168nEnabled_ && addr <= 0x3FFF) {
    return ce168nRam_[static_cast<size_t>(ce168nBank_) * 0x4000 + addr];
  }
  if (isUnmapped(addr)) return 0xFF;
  return me0_[effectiveAddr(addr, machineVariant_)];
}

void Bus::writeME0(uint16_t addr, uint8_t value) {
  lastAccessedSpace_ = MemorySpace::ME0;
  if (addr >= 0x8000 && addr <= 0xBFFF) {
    // Unlike a plain loadrommodule-style module (whose whole claimed range
    // is real ROM and can never be written -- isUnmapped's own blanket
    // "CE-150/153/158 (not connected)" discard below still applies to
    // those), a module with hasDataWindow genuinely has RAM there on real
    // hardware. A write landing on that module's own instructionAddr
    // additionally triggers mock command processing, mirroring the real
    // board's DoCommand()/WriteStatus() protocol (write a command byte,
    // poll the same address for a non-BUSY status) -- see ExpansionMock.
    for (RomModule& m : romModules_) {
      if (!m.tryWrite(addr, pv_, value)) continue;
      if (m.hasDataWindow && addr == m.instructionAddr) {
        uint8_t status = expansionMock_.processCommand(value, m.dataWindow);
        m.dataWindow[m.instructionAddr - m.dataWindowBase] = status;
      }
      return;
    }
    return;  // not claimed by any module -- isUnmapped's own discard below covers this range anyway
  }
  // CE-163 bank-select trigger: pin 18 = S3 -> 5800H-5FFFH on a PC-1500,
  // or the same physical pin wired to S5 -> 6800H-6FFFH on a PC-1500A
  // (see Bus::setCe163Enabled's own comment). Pure address-line latch on
  // real hardware -- the byte value is irrelevant, and nothing is
  // actually stored at this address.
  {
    uint16_t triggerBase = machineVariant_ == MachineVariant::PC1500A ? 0x6800 : 0x5800;
    if (ce163Enabled_ && addr >= triggerBase && addr <= triggerBase + 0x7FF) {
      ce163Bank_ = static_cast<uint8_t>(addr & 1);
      return;
    }
  }
  // CE-163 data write -- see readME0's own comment for why this bypasses
  // me0_ and goes straight to the bank-selected slice of ce163Ram_.
  if (ce163Enabled_ && addr <= 0x3FFF) {
    ce163Ram_[static_cast<size_t>(ce163Bank_) * 0x4000 + addr] = value;
    return;
  }
  // CE-168N bank-select trigger, same range as CE-163 (same physical port,
  // same machine-variant dependence -- 5800H-5FFFH on a PC-1500, or
  // 6800H-6FFFH on a PC-1500A). `%` rather than `&`, since the bank count
  // is an arbitrary parameter, not guaranteed to be a power of two.
  {
    uint16_t triggerBase = machineVariant_ == MachineVariant::PC1500A ? 0x6800 : 0x5800;
    if (ce168nEnabled_ && addr >= triggerBase && addr <= triggerBase + 0x7FF) {
      ce168nBank_ = static_cast<uint8_t>(addr % ce168nBanks_);
      return;
    }
  }
  // CE-168N data write -- a bank at or above ce168nFirstRoBank_ simulates
  // flash: the write is silently discarded rather than erroring, matching
  // a flash chip ignoring an unprogrammed write pulse. Initial content for
  // a read-only bank comes from loadME0 (the `loadbinary` FIFO command),
  // not this CPU-write path -- see Bus::loadME0's own comment.
  if (ce168nEnabled_ && addr <= 0x3FFF) {
    if (ce168nBank_ < ce168nFirstRoBank_) {
      ce168nRam_[static_cast<size_t>(ce168nBank_) * 0x4000 + addr] = value;
    }
    return;
  }
  if (isUnmapped(addr) || isRom(addr)) return;
  me0_[effectiveAddr(addr, machineVariant_)] = value;
}

namespace {
// CS0/CS1/CS2 are tied to AD12/AD13/(fixed) -- AD14/AD15 aren't part of the
// decode. Confirmed on real hardware: F00AH/F00BH and B00AH/B00BH read back
// identical, live values (F000H=1111..., B000H=1011... -- they differ only
// in bit 14). So the controller is selected whenever bits 12-13 are both
// set, regardless of bits 14-15 (or bits 4-11, which are likewise unused --
// only RS0-RS3, bits 0-3, select the register).
bool IoControllerSelected(uint16_t addr) { return (addr & 0x3000) == 0x3000; }
}  // namespace

uint8_t Bus::readME1(uint16_t addr) {
  lastAccessedSpace_ = MemorySpace::ME1;
  if (IoControllerSelected(addr)) return io_.read(static_cast<uint8_t>(addr & 0x0F));
  return 0xFF;
}

void Bus::writeME1(uint16_t addr, uint8_t value) {
  lastAccessedSpace_ = MemorySpace::ME1;
  if (IoControllerSelected(addr)) io_.write(static_cast<uint8_t>(addr & 0x0F), value);
}

uint8_t Bus::readInputPort() { return keyboard_.scan(io_.opaOutput()); }

namespace {
bool isCursorKey(Key key) {
  return key == Key::Left || key == Key::Right || key == Key::Up || key == Key::Down ||
         key == Key::UpDownRocker;
}
}  // namespace

void Bus::setKeyState(Key key, bool pressed) {
  if (pressed) {
    // A fresh press cancels any release we were about to apply for this
    // same key (host OS key-repeat, or a very fast re-tap) -- it's back
    // down, so there's nothing left to release.
    for (auto it = pendingReleases_.begin(); it != pendingReleases_.end(); ++it) {
      if (it->key == key) {
        pendingReleases_.erase(it);
        break;
      }
    }
    keyboard_.setKeyState(key, true);
    if (isCursorKey(key)) {
      // Every call here now represents a genuine fresh press -- the host
      // event loop filters OS-level key-repeat before it ever reaches
      // setKeyState (see main.cpp), so there's no "is this just an OS
      // repeat of an already-held key" case left to distinguish here.
      // Always resetting is also *correct* where the old conditional
      // wasn't: a quick tap's deferred release (see below) can still be
      // pending when a genuinely new, separate tap of the same key
      // arrives, which left cursorKeyHeld_/heldCursorKey_ looking
      // unchanged and wrongly carried over the first tap's accumulated
      // cursorRepeatCycles_ -- if that pushed the total over
      // kCursorRepeatCycles, the second tap got a spurious extra
      // "repeat" injected on top of its own real press, i.e. exactly the
      // double-press bug this fixes.
      cursorRepeatCycles_ = 0;
      cursorRepeatFired_ = false;
      cursorKeyHeld_ = true;
      heldCursorKey_ = key;
    }
    return;
  }

  // Defer the actual release: the ROM only polls the keyboard once per
  // timer-interrupt period (~25ms). A host keypress briefer than that
  // (or one whose press and release both land in the same emulated cycle
  // batch) could otherwise go all the way down and back up without any
  // ROM scan ever seeing it. Holding it down a little longer than the
  // host actually did guarantees at least one scan period sees it.
  pendingReleases_.push_back({key, kMinimumHoldCycles});
}

void Bus::applyRelease(Key key) {
  keyboard_.setKeyState(key, false);
  if (isCursorKey(key)) {
    if (key == heldCursorKey_) cursorKeyHeld_ = false;
  }
  // No forced 7B0EH-bit-0 clear here for ordinary keys anymore -- see
  // setKeyState's comment. That was compensating for the CPU timer being
  // modeled as a linear counter instead of the real 9-bit polynomial one
  // (fixed 2026-08-02, lh5801_timer_table.h); with the real polynomial
  // sequence, the ROM's own countdown clears the gate within a few
  // thousand cycles on its own (confirmed by direct trace, well under one
  // ~32700-cycle timer period), not the ~200ms the old linear model
  // implied. The forced clear was actively wrong, not just redundant: it
  // broke BASWORD's own keyboard-hook timing (confirmed -- disabling it
  // is what made `BASWORD +"name";"..."` start actually registering
  // keywords, see [[pc1500_keyword_table_mechanism]]).
}

void Bus::advanceCycles(int cycles) {
  io_.advanceCycles(cycles);
  if (cursorKeyHeld_) {
    cursorRepeatCycles_ += cycles;
    int threshold = cursorRepeatFired_ ? kCursorRepeatCycles : kCursorInitialDelayCycles;
    if (cursorRepeatCycles_ >= threshold) {
      cursorRepeatCycles_ = 0;
      cursorRepeatFired_ = true;
      writeME0(0x7B0E, static_cast<uint8_t>(readME0(0x7B0E) & 0xFE));
    }
  }
  for (auto it = pendingReleases_.begin(); it != pendingReleases_.end();) {
    it->cyclesRemaining -= cycles;
    if (it->cyclesRemaining <= 0) {
      applyRelease(it->key);
      it = pendingReleases_.erase(it);
    } else {
      ++it;
    }
  }
}

void Bus::loadME0(uint16_t addr, const uint8_t* data, size_t size) {
  // CE-168N: this is the scripting/preset load path (backs the `loadbinary`
  // FIFO command), as opposed to writeME0's CPU-write path -- it writes
  // into the currently selected bank's storage unconditionally, bypassing
  // the read-only-bank check, so a preset can seed a flash bank's initial
  // content (select it first via the bank-select trigger, same as any
  // other bank) before the ROM ever runs. Bytes beyond addr<=0x3FFF still
  // fall through to me0_ below, same as an ordinary loadbinary call.
  if (ce168nEnabled_ && addr <= 0x3FFF) {
    for (size_t i = 0; i < size; i++) {
      uint32_t target = static_cast<uint32_t>(addr) + i;
      if (target > 0x3FFF) break;
      ce168nRam_[static_cast<size_t>(ce168nBank_) * 0x4000 + target] = data[i];
    }
    return;
  }
  for (size_t i = 0; i < size; i++) {
    uint32_t target = static_cast<uint32_t>(addr) + i;
    if (target > 0xFFFF) break;
    me0_[static_cast<uint16_t>(target)] = data[i];
  }
}

namespace {
constexpr size_t kSavedRamSize = 0x8000;  // me0_[0x0000,0x8000) -- see Bus::saveState's comment

void saveRomModule(std::ostream& os, const Bus::RomModule& m) {
  using namespace pc1500state;
  writeU32(os, static_cast<uint32_t>(m.data.size()));
  writeBytes(os, m.data.data(), m.data.size());
  writeU16(os, m.base);
  writeBool(os, m.requirePv);
  writeBool(os, m.usePuBank);
  // Appended fields -- an older save file lacks these entirely, so loading
  // one with newer code will misread past the end of the module's own
  // data; accepted for this pre-1.0 dev tool rather than adding a format
  // version just for this.
  writeBool(os, m.hasDataWindow);
  writeU32(os, static_cast<uint32_t>(m.dataWindow.size()));
  writeBytes(os, m.dataWindow.data(), m.dataWindow.size());
  writeU16(os, m.dataWindowBase);
  writeU16(os, m.instructionAddr);
}

bool loadRomModuleState(std::istream& is, Bus::RomModule& m) {
  using namespace pc1500state;
  uint32_t size = readU32(is);
  m.data.assign(size, 0);
  readBytes(is, m.data.data(), m.data.size());
  m.base = readU16(is);
  m.requirePv = readBool(is);
  m.usePuBank = readBool(is);
  m.hasDataWindow = readBool(is);
  uint32_t windowSize = readU32(is);
  m.dataWindow.assign(windowSize, 0);
  readBytes(is, m.dataWindow.data(), m.dataWindow.size());
  m.dataWindowBase = readU16(is);
  m.instructionAddr = readU16(is);
  return !is.fail();
}
}  // namespace

void Bus::saveState(std::ostream& os) const {
  using namespace pc1500state;
  // Config scalars first (version 6 -- see state_file.h's own comment for
  // why: loadState needs to check these against its *current* values
  // before committing to restoring the large blobs below, so a config
  // mismatch can be rejected without touching anything).
  writeU32(os, static_cast<uint32_t>(extRamExtSize_));
  writeU32(os, static_cast<uint32_t>(extRam0000Size_));
  writeBool(os, ce163Enabled_);
  writeU8(os, ce163Bank_);
  writeBool(os, machineVariant_ == MachineVariant::PC1500A);
  writeBool(os, ce155Enabled_);
  writeBytes(os, me0_.data(), kSavedRamSize);
  writeBytes(os, ce163Ram_.data(), ce163Ram_.size());
  // Version 6 addition -- inserted here (rather than appended at the very
  // end, unlike version 5's own fields below) to keep it next to the
  // CE-163 fields it parallels.
  writeBool(os, ce168nEnabled_);
  writeU8(os, ce168nBank_);
  writeU8(os, ce168nBanks_);
  writeU8(os, ce168nFirstRoBank_);
  writeBytes(os, ce168nRam_.data(), ce168nRam_.size());  // size implied by ce168nBanks_ above
  for (const RomModule& m : romModules_) {
    saveRomModule(os, m);
  }
}

bool Bus::loadState(std::istream& is, std::string* error, bool* configMismatch,
                     SavedConfig* savedConfig) {
  using namespace pc1500state;
  size_t savedExtRamExtSize = readU32(is);
  size_t savedExtRam0000Size = readU32(is);
  bool savedCe163Enabled = readBool(is);
  uint8_t savedCe163Bank = readU8(is);
  MachineVariant savedVariant =
      readBool(is) ? MachineVariant::PC1500A : MachineVariant::PC1500;
  bool savedCe155Enabled = readBool(is);
  if (!is) {
    if (error) *error = "truncated or corrupt (couldn't even read the config header)";
    return false;
  }
  // Reject outright, touching nothing else, if the saved config doesn't
  // match what this Bus is currently configured for -- see loadState's
  // own header-comment for why this matters (raw me0_ contents are only
  // meaningful relative to the specific memory-map shape they were saved
  // under). The caller is responsible for having already applied the
  // intended config before calling this.
  if (savedExtRamExtSize != extRamExtSize_ || savedExtRam0000Size != extRam0000Size_ ||
      savedCe163Enabled != ce163Enabled_ || savedVariant != machineVariant_ ||
      savedCe155Enabled != ce155Enabled_) {
    if (error) {
      *error =
          "state file's saved RAM configuration doesn't match the currently configured "
          "hardware -- apply the same Settings (base unit / extension RAM) the state was "
          "saved under before loading it, the same way real PC-1500 RAM (short of a "
          "battery-backed module) doesn't survive a hardware reconfiguration either";
    }
    if (configMismatch) *configMismatch = true;
    if (savedConfig) {
      savedConfig->extRamExtSize = savedExtRamExtSize;
      savedConfig->extRam0000Size = savedExtRam0000Size;
      savedConfig->ce163Enabled = savedCe163Enabled;
      savedConfig->machineVariant = savedVariant;
      savedConfig->ce155Enabled = savedCe155Enabled;
    }
    return false;
  }
  ce163Bank_ = savedCe163Bank;
  readBytes(is, me0_.data(), kSavedRamSize);
  readBytes(is, ce163Ram_.data(), ce163Ram_.size());
  ce168nEnabled_ = readBool(is);
  ce168nBank_ = readU8(is);
  ce168nBanks_ = readU8(is);
  ce168nFirstRoBank_ = readU8(is);
  ce168nRam_.assign(static_cast<size_t>(ce168nBanks_) * 0x4000, 0);
  readBytes(is, ce168nRam_.data(), ce168nRam_.size());
  bool ok = true;
  for (RomModule& m : romModules_) {
    ok = loadRomModuleState(is, m) && ok;
  }
  // Still worth doing even though config matched: a state saved under a
  // matching config, but with an older build that predates this method
  // existing (or predates reserveAreaBase() following the config at all),
  // can still have its live reserve area sitting at the generic,
  // never-seeded 0xFF fill -- see reseedReserveArea's own comment.
  reseedReserveArea();
  if (!ok || !is) {
    if (error) *error = "truncated or corrupt";
    return false;
  }
  return true;
}

}  // namespace pc1500
