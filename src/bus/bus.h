// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iosfwd>
#include <vector>

#include "expansion_mock.h"
#include "keyboard.h"
#include "lh5801.h"

namespace pc1500 {

// NEC uPD1990AC real-time clock chip, wired to the LH5811 I/O port
// controller's PC0-PC5 (control) and PB5/PB6 (status input) pins -- see
// docs/pc1500_hardware_reference.md and Documents/PC1500/UPD1990AC.pdf
// (the real datasheet). A 40-bit BCD shift register holds
// month/day-of-week/day/hour/minute/second; C0-C2 (latched by STB) select
// one of Register Hold/Shift/Time-Set/Time-Read (C2=0) or a TP output rate
// of 64/256/2048 Hz (C2=1) -- two independently-latched command groups,
// per the datasheet's own wording.
//
// Confirmed wired exactly this way via direct ROM1.BIN disassembly: BASIC
// BEEP's repeat-gap wait (E890-E8B4) issues value 0x20 through the shared
// "set OPC low 6 bits + pulse STB" helper at E573 -- i.e. C2=1,C1=0,C0=0 on
// PC5/PC4/PC3 -- which is exactly the datasheet's "TP=64Hz Set" command,
// before polling PB5 (TP's live level) and IF bit 1 (latched from TP's
// rising edge). That's what resolved the mystery of what BEEP's gap
// timer actually depends on -- and, later, of why the *emulator's* WAIT n
// exited early with a spurious "BREAK IN n": WAIT's own ROM poll loop
// (LE89C-LE8BC in _bisect/rom1.asm) busy-polls PB5/IF at CPU-instruction
// granularity, expecting to see every individual TP rising edge, while an
// earlier version of this class only updated tpLevel_/IF once per
// *rendered frame* (~16.7ms) via a batched advanceRealTime(elapsedSeconds)
// call. Since one frame can span multiple TP half-periods (64Hz's own
// half-period is ~7.8ms), that batch update could process a full
// low->high->low excursion atomically inside one call, latching IF bit 1
// for the edge but leaving PB5 back at low by the time any CPU instruction
// actually ran and could observe it -- a transition genuinely invisible to
// a busy-poll loop, no matter how tightly it polled, and indistinguishable
// from a stale leftover flag from an earlier tick. Fixed by computing TP's
// level *lazily*, from real elapsed wall-clock time, every time it's
// actually read (see tp()/consumeRisingEdge()) rather than in per-frame
// batches -- still driven by real time (this chip has its own independent
// 32.768kHz crystal per the Service Manual's block diagram, so its rate
// must stay correct regardless of the emulated CPU's own speed), just
// checked at whatever granularity code actually polls it, so no edge can
// ever occur without some read eventually being the one to observe it.
//
// A second, subtler bug survived that first fix: OPB (PB5, level) and IF
// bit 1 (edge latch) are two *different* views of the same signal -- OPB
// answers "is TP high right now", IF answers "has TP gone high since I
// last checked" -- and LE89C-LE8BC's own structure depends on both
// answers independently and correctly: it busy-spins on OPB alone while
// TP is genuinely still high (thousands of CPU cycles, ~7.8ms at 64Hz),
// only falling back to IF when OPB reads low, on the assumption that IF
// being set at that specific moment can only mean BREAK, since a genuine
// tick would already show up via OPB. An earlier version of
// IoPortController::read()'s OPB case (F00FH) helpfully also latched a
// caught edge into IF bit 1 for a later IF read's benefit -- but that
// left the bit sitting there, unconsumed, for the *entire* rest of that
// tick's high phase, so the very next busy-spin iteration's IF check
// (there is one on every iteration OPB reads high, since it's the only
// way to distinguish "still waiting" from "BREAK") found it stale-set
// and took the abort path (LE8B4) -- one tick after every single
// decrement, not just the first. Fixed by not sharing state between the
// two register reads at all: IF bit 1's RTC contribution is now computed
// entirely within IoPortController::read()'s own IF case (0x0B), via its
// own independent consumeRisingEdge() call, so OPB's read (case 0x0F)
// has nothing to latch and nothing to leave stale.
//
// That still left a narrow race in the *opposite* direction: if a rising
// edge lands in the handful of CPU cycles between OPB's own read (finding
// TP still low) and IF's fallback read a few instructions later, IF can
// genuinely, correctly see a fresh edge OPB itself hasn't observed yet --
// which LE8A9's branch structure reads as BREAK. Measured live
// (testWaitPollTrace, direct instrumentation) at ~61us between the two
// actual register reads (OPB's bii, then a vmj plus the E451 helper's own
// bii+rtn before IF's read happens), against a ~15us OPB-to-OPB full loop
// iteration -- initially assumed rare relative to TP's ~7.8ms
// half-period, but with a fully deterministic clock and a fixed-cycle
// poll loop this isn't actually rare at all: whatever phase relationship
// exists between the loop's own cadence and a tick's arrival repeats
// identically on every tick within one WAIT/BEEP call, so an unlucky
// alignment fails *every* tick, confirmed live (e8bcCount stuck at 1-2
// regardless of the requested duration). A fixed time grid was tried next
// (round both reads' timestamps to a shared quantum before comparing) and
// made it *worse* for the same determinism reason, just relocating which
// phase is unlucky rather than removing the phenomenon. What actually
// fixes it: consumeRisingEdge() (IF's read) is debounced specifically
// against tp()'s (OPB's read) own most recent timestamp, not a shared
// grid -- if OPB was checked within the last kTpResyncDebounceSeconds, IF
// trusts what OPB already established instead of independently
// resampling, guaranteeing the two agree by construction regardless of
// absolute phase, since OPB is always the earlier of the two within one
// poll iteration. OPB's own read is never debounced (always fresh), so it
// can never itself go stale the way a shared-window approach caused.
class Upd1990ac {
 public:
  // Forward every OPC write here with its six control-line levels
  // (PC0=DATA IN, PC1=STB, PC2=CLK, PC3=C0, PC4=C1, PC5=C2 -- confirmed via
  // the E573 ROM helper toggling bit 1 for STB pulses). Real behavior only
  // happens on STB/CLK rising edges, detected internally, so callers don't
  // need to track state themselves.
  void setControlPins(bool dataIn, bool stb, bool clk, bool c0, bool c1, bool c2);

  // PB5 (TP)'s bit as OPB (F00FH) should report it: the live level,
  // OR'd with any not-yet-independently-observed pending edge (so a read
  // landing in the handful of CPU cycles between a rising edge and TP's
  // live level catching up still sees it as high -- the fix for WAIT n's
  // ROM poll loop, LE89C-LE8BC in _bisect/rom1.asm, missing individual TP
  // edges that an earlier per-frame-batched model could hide between
  // polls). Lazily recomputed from real elapsed wall-clock time since TP
  // was last (re-)configured (see latchCommand) -- see this class's own
  // comment for why lazy-at-read beats a periodic batch update. Always a
  // genuine fresh resample, never debounced -- see consumeRisingEdge()'s
  // own comment for why OPB specifically must never be allowed to go
  // stale, and why it's the one other reads are debounced against rather
  // than the reverse. const (mutates the internal tpLevel_/tpEdgePending_
  // cache plus lastOpbSyncTime_/everOpbSynced_, all mutable below, same
  // pattern as IoPortController::if_).
  bool tp() const;

  // PB6 (DATA OUT) live level -- see IoPortController::read()'s OPB case,
  // which reads both this and tp() unconditionally regardless of DDB (same
  // treatment as PB7/onKeyLine_, since these are fixed hardwired
  // connections on a stock PC-1500, not general-purpose I/O).
  bool dataOut() const;

  // Returns true, and clears the pending flag, if a TP rising edge has
  // occurred (per real elapsed wall-clock time) since the last call to
  // either this or tp() -- called independently by both
  // IoPortController::read()'s OPB case (F00FH, so a read landing exactly
  // on the edge still sees TP as high) and its IF case (F00BH bit 1, so
  // IF alone can detect a fresh edge with no OPB read involved at all --
  // see that case's own comment for why the two reads deliberately don't
  // share state beyond both drawing on this same method). Also recomputes
  // tp()'s cached level as a side effect (both draw from the same lazy
  // resync), so callers don't need to call tp() first.
  //
  // Returns false unconditionally until the ROM has issued at least one
  // TP-rate-select command (see latchCommand) -- confirmed necessary by a
  // real regression: without this gate, TP started toggling from process
  // launch using a guessed default rate, which spuriously set IF bit 1
  // during the boot ROM's own "NEW0?:CHECK" prompt sequence (well before
  // BEEP or anything else ever issues a real TP command), skipping past a
  // prompt that real hardware correctly stops at. The datasheet doesn't
  // document TP's power-on-reset mux state, but real hardware clearly
  // doesn't have this problem despite the RTC's divider presumably having
  // been free-running for years off a backup battery -- so a raw,
  // ungated TP-to-IF-bit-1 mirror can't be the whole story; not letting
  // TP mean anything before it's actually configured is the simplest fix
  // that resolves the regression while keeping BEEP working (it always
  // configures TP=64Hz before ever polling).
  bool consumeRisingEdge() const;

  // Explicitly (re-)syncs this chip's notion of "now" to the host's
  // current wall-clock time -- the same effect BASIC's own TIME command
  // has (see commitShiftRegisterToTime()), without needing the CPU to
  // actually run any ROM code to get there. Already the default at
  // construction (timeOffset_ starts at 0, see its own comment), so
  // this mostly exists to make that intent explicit and give a caller
  // (see main.cpp) an obvious hook to call at startup, rather than
  // relying on nobody having reset timeOffset_ away from its default in
  // the meantime.
  void syncToHostClock() { timeOffset_ = std::chrono::seconds{0}; }

  // TEMPORARY debug accessors for the DungeonQuest.bas WAIT-hang
  // investigation -- remove once root-caused. Non-invasive (plain const
  // reads, no behavior change).
  bool debugTpConfigured() const { return tpConfigured_; }
  int debugTpRateHz() const { return tpRateHz_; }
  bool debugTpLevel() const { return tp(); }
  int debugTotalToggleCount() const { return debugTotalToggleCount_; }

  // Switches syncTp()'s notion of "now" from directly reading the real
  // host clock to a manually-advanced one (see now()/advanceManualClock()),
  // re-anchored to the real host clock's current instant right now. Used
  // in two different calling patterns:
  //
  // - Tests call this *once*, before driving any instructions, then only
  //   ever move time forward via advanceManualClock() -- a fully frozen,
  //   deterministic clock. Without this, a test driving many instructions
  //   between calls would have real wall-clock time (including the
  //   test's own instrumentation/printf overhead) bleed into the RTC's
  //   state on top of whatever the test itself intended to simulate --
  //   confirmed as a real source of nondeterminism (the same test
  //   producing different traces run to run) before this existed.
  // - Production (main.cpp) calls this *once per rendered frame*, then
  //   advances it after every CPU instruction stepped within that frame
  //   by that instruction's own cycle cost (see advanceManualClock()).
  //   This is not just a style choice: a rendered frame's entire CPU
  //   cycle budget executes in a small fraction of a millisecond on any
  //   modern host (a simple interpreted 8-bit core can retire tens of
  //   millions of instructions per second), so reading the real host
  //   clock directly on every register access -- as this class did before
  //   -- means every read within one frame's burst sees essentially the
  //   *same* timestamp. WAIT/BEEP's tight ROM poll loop (LE89C-LE8BC in
  //   _bisect/rom1.asm) can then go an entire frame (~16.7ms) without
  //   ever observing a TP transition (~7.8ms half-period at 64Hz) at all
  //   -- and since a frame period is close to *two* TP half-periods,
  //   transitions frequently net-cancel between one frame's single
  //   snapshot and the next, so WAIT n could complete many times slower
  //   than its true n/64 seconds (confirmed live: WAIT 64 took roughly
  //   10 real seconds instead of 1). Re-anchoring to the real clock once
  //   per frame (rather than leaving it to drift on cycle-count alone)
  //   keeps this from accumulating error against real wall-clock time
  //   over a long session, the same way the audio sample position is
  //   re-anchored to real elapsed time every frame rather than purely
  //   interpolated from cycle count.
  void useManualClock() {
    manualClockActive_ = true;
    manualClockValue_ = std::chrono::steady_clock::now();
  }

  // Advances the manual clock (see useManualClock()) by `seconds`,
  // without actually sleeping and without any real elapsed time bleeding
  // in between calls. Tests call this with a fixed simulated duration;
  // production calls this after every CPU instruction, scaled by that
  // instruction's own cycle count over the emulated clock rate (see
  // main.cpp's kCyclesPerSecond), so real-time-dependent reads within a
  // frame see smooth, monotonic progress instead of one frozen snapshot.
  void advanceManualClock(double seconds) {
    manualClockValue_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
  }

 private:
  // now() abstracts the clock source syncTp() uses -- real host time by
  // default, a manually-advanced clock once useManualClock() has been
  // called (see its own comment for the two different ways that's used).
  std::chrono::steady_clock::time_point now() const {
    return manualClockActive_ ? manualClockValue_ : std::chrono::steady_clock::now();
  }

  // Recomputes tpLevel_/tpEdgePending_ from real elapsed wall-clock time
  // since tpEpoch_ -- shared by tp() and consumeRisingEdge() so they never
  // disagree about the current state. const (see tpLevel_ etc.'s own
  // mutable comment).
  void syncTp() const;
  // Per the datasheet's command table: Group 0 (C2=0) selects one of these
  // four register-control modes; Group 1 (C2=1, handled separately via
  // tpRateHz_) selects the TP output rate instead.
  enum class Mode { RegisterHold, RegisterShift, TimeSet, TimeRead };

  void latchCommand(bool c0, bool c1, bool c2);
  uint64_t liveTimeAsBcd40() const;
  void commitShiftRegisterToTime();

  bool prevStb_ = false;
  bool prevClk_ = false;
  bool dataIn_ = false;

  Mode mode_ = Mode::RegisterHold;
  int tpRateHz_ = 64;
  bool tpConfigured_ = false;  // see consumeRisingEdge()
  // 40 bits used: bits [3:0]=units-of-seconds ... bits [39:36]=month (see
  // liveTimeAsBcd40()). Shifts right on each CLK edge while mode_ is
  // RegisterShift or TimeSet (the two modes the datasheet lists as
  // "DATA OUT=[LSB]"); bit 0 is what falls out to DATA OUT (Fig. 1: "DATA
  // ... appears on DATA OUT terminal from LSB of Second"), and the new bit
  // from DATA IN enters at bit 39.
  uint64_t shiftRegister_ = 0;

  // TP's real-time reference point: reset to now() every time TP is
  // (re-)configured (see latchCommand), so tpLevel_ can be recomputed on
  // demand as a pure function of elapsed wall-clock time since then --
  // see syncTp(). mutable since tp()/consumeRisingEdge() are const (same
  // read-triggers-a-side-effect pattern as IoPortController::if_).
  mutable std::chrono::steady_clock::time_point tpEpoch_{};
  mutable bool tpLevel_ = false;
  mutable bool tpEdgePending_ = false;
  mutable int debugTotalToggleCount_ = 0;  // TEMPORARY

  // Records when tp() (OPB's read) last actually resampled -- see
  // consumeRisingEdge()'s own comment for why IF's read is debounced
  // against *this* specifically. everOpbSynced_ starts false (and is
  // reset false whenever TP is (re-)configured, alongside tpEpoch_ -- see
  // latchCommand) so consumeRisingEdge() never trusts a stale
  // pre-(re-)configure timestamp.
  mutable std::chrono::steady_clock::time_point lastOpbSyncTime_{};
  mutable bool everOpbSynced_ = false;

  // See useManualClock()/now().
  mutable bool manualClockActive_ = false;
  mutable std::chrono::steady_clock::time_point manualClockValue_{};

  // now() + timeOffset_ is this chip's notion of "now". Starts at 0 (i.e.
  // matches host wall-clock time) rather than an arbitrary epoch, so
  // BASIC's TIME$/DATE$ show something sensible even before any Time-Set
  // command -- like a real battery-backed clock that happens to already
  // read correctly. Time-Set commits a new value here (see
  // commitShiftRegisterToTime()); the chip has no year field at all (per
  // the datasheet's 40-bit layout), so the host's current year is always
  // used for that part.
  std::chrono::seconds timeOffset_{0};
};

// LH5810/LH5811 I/O port controller, mapped into ME1 at F000H-F00FH (see
// docs/pc1500_hardware_reference.md). Register selected by the low 4 bits
// of the address (RS3-RS0). Only the registers actually used on a stock
// PC-1500 are modeled with real behavior (DDA/OPA for keyboard column
// strobe, OPB for the ON-key mirror on PB7); the rest are plain
// read/write storage with no further side effects yet.
class IoPortController {
 public:
  uint8_t read(uint8_t reg) const;
  void write(uint8_t reg, uint8_t value);

  // PA0-PA7 output value, gated by DDA (bits set to input read back as 1,
  // matching an undriven/pulled-up line) -- this is what the keyboard's
  // column strobe sees.
  uint8_t opaOutput() const;

  // PB7 is hardwired as the ON-key input regardless of DDB (see
  // docs/pc1500_hardware_reference.md) -- live level, not a latch, so it
  // tracks the physical key state directly. On a rising edge, also
  // latches IF bit 0x02 (kTpFlagBit -- confirmed shared with the RTC's TP
  // edge, see its own comment below) -- PB7 is configured as an LH5811
  // interrupt input, and this is the flag real hardware sets so the CPU's
  // MI handler notices. Confirmed by tracing backward from the literal
  // "BREAK IN" string in ROM1.BIN's message table to find what actually
  // triggers it: setting only this bit, or only requesting MI without it,
  // was each independently confirmed insufficient -- a running BASIC
  // program only actually breaks with both together.
  void setOnKeyLine(bool pressed) {
    if (pressed && !onKeyLine_) if_ |= kTpFlagBit;
    onKeyLine_ = pressed;
  }

  // OPC bit 6 (PC6) is the buzzer on/off control -- there's no separate
  // tone/frequency register on real hardware; BASIC's BEEP works by
  // having the ROM toggle this bit in a timed software loop, and the
  // resulting square wave *is* the tone. Emulating it is therefore just
  // exposing the bit's live level for the host audio callback to sample
  // directly (see main.cpp) -- std::atomic since that callback runs on
  // SDL's own audio thread, not the emulator's main thread that calls
  // write().
  bool buzzerOn() const { return buzzerOn_.load(std::memory_order_relaxed); }

  // Drives serial-transmit timing (see write()'s 0x06 case): call once per
  // emulated cycle batch, same as Bus::advanceCycles (which forwards to
  // this). Counts down txCyclesRemaining_ and sets the TD flag (IF bit 3)
  // when a transmission completes.
  void advanceCycles(int cycles);

  // Non-consuming peek at the RTC's TP output level -- main.cpp's frame
  // loop calls this once per rendered frame (~16.7ms), a cadence entirely
  // uncorrelated with WAIT/BEEP's own CPU-instruction-granularity poll
  // loop (LE89C-LE8BC in _bisect/rom1.asm). An earlier version of this
  // consumed a pending edge and latched it into IF as a side effect --
  // which meant this call could "steal" a tick out from under the ROM's
  // own poll loop (or latch it into IF well before/after the ROM's own
  // OPB read would have), independently of and inconsistently with
  // whatever the CPU's own reads were doing -- reintroducing, at frame
  // granularity, the exact same class of missed/duplicated-edge bug the
  // lazy-per-read rework (see Upd1990ac's class comment) was meant to
  // fix at the OPB/IF-register level. Kept as a public entry point mainly
  // so main.cpp's frame loop (and tests) can force a sync even in
  // stretches with no register reads at all; genuinely harmless to call
  // as often or as rarely as convenient now that it has no side effects.
  bool pollRtc() { return rtc_.tp(); }

  // Forwards to the RTC -- see Upd1990ac::syncToHostClock().
  void syncRtcToHostClock() { rtc_.syncToHostClock(); }

  // TEMPORARY, see Upd1990ac's own debug accessors -- remove together.
  bool rtcDebugTpConfigured() const { return rtc_.debugTpConfigured(); }
  int rtcDebugTpRateHz() const { return rtc_.debugTpRateHz(); }
  bool rtcDebugTpLevel() const { return rtc_.debugTpLevel(); }
  int rtcDebugTotalToggleCount() const { return rtc_.debugTotalToggleCount(); }

  // Forward to Upd1990ac::useManualClock()/advanceManualClock() -- see
  // their own comments for the two different ways these get used (a
  // one-time freeze for tests; a once-per-frame resync plus
  // per-instruction advance in production). Call useManualRtcClock()
  // before any advanceManualRtcClock() calls, or real elapsed wall-clock
  // time (including, in a test, the test's own instrumentation overhead)
  // will keep bleeding in on top of what the caller intends.
  void useManualRtcClock() { rtc_.useManualClock(); }
  void advanceManualRtcClock(double seconds) { rtc_.advanceManualClock(seconds); }
  int debugOpcWriteCount() const { return debugOpcWriteCount_; }
  uint8_t debugLastOpcValue() const { return debugLastOpcValue_; }
  const std::vector<uint8_t>& debugOpcWriteHistory() const { return debugOpcWriteHistory_; }

 private:
  uint8_t dda_ = 0;
  uint8_t ddb_ = 0;
  uint8_t opa_ = 0;
  uint8_t opb_ = 0;
  uint8_t opc_ = 0;
  int debugOpcWriteCount_ = 0;    // TEMPORARY, see debugOpcWriteCount()
  uint8_t debugLastOpcValue_ = 0;  // TEMPORARY, see debugLastOpcValue()
  std::vector<uint8_t> debugOpcWriteHistory_;  // TEMPORARY, see debugOpcWriteHistory()
  uint8_t f_ = 0;
  uint8_t g_ = 0;
  uint8_t msk_ = 0;
  // mutable: reading U (register 0x05) clears the RD flag here as a real
  // hardware side effect (see below) even though read() is logically const
  // from the CPU's point of view (a memory-mapped register read, not a
  // write instruction).
  mutable uint8_t if_ = 0;
  bool onKeyLine_ = false;
  std::atomic<bool> buzzerOn_{false};

  // LH5811 serial transmission (RS=0110, "send data" trigger register --
  // called L in the PC-2 Service Manual's I/O port controller block
  // diagram) and reception (RS=0101, U register). Confirmed real usage via
  // CE-150 (printer/cassette interface) ROM disassembly: it polls IF bit 3
  // (0x08) clear before writing a new byte to 0x06, and the byte written
  // there is never itself readable back (register is write-only/trigger,
  // per the Service Manual's register table) -- i.e. bit 3 is TD
  // (transmit done), set by hardware when a transmission finishes, and
  // implicitly clear while one is in progress. Confirmed BASIC's BEEP does
  // NOT use this path at all (it bit-bangs OPC directly and waits on IF
  // bit 1, which tracks something else entirely -- see docs/pc1500_hardware_reference.md).
  //
  // RD (receive done) bit position is *not* confirmed against any ROM
  // disassembly (no PC-1500 ROM code path exercising CLOAD-style serial
  // reception has been traced yet) -- bit 2 here is a best guess based on
  // typical IRQ/PB7/RD/TD bit ordering, not a verified hardware fact.
  static constexpr uint8_t kTdFlagBit = 0x08;
  static constexpr uint8_t kRdFlagBit = 0x04;  // unconfirmed -- see above

  // Transmission format is start bit + 8 data bits + 2 stop bits (11 bit
  // periods total, per the Service Manual), each bit period lasting
  // divisor() cycles of the CPU's own clock (the "basic clock" the divisor
  // list is stated relative to). The 3-bit divisor selector packed into F
  // bits 2-0 and this ascending-rate ordering are also not confirmed
  // against a real bit-encoding table (none survived OCR from either
  // manual) -- best guess from the Service Manual's listed rate order,
  // refinable later against real CE-150 ROM timing if it matters.
  uint8_t transmitClockDivisor() const {
    static constexpr int kDivisors[8] = {1, 2, 128, 256, 512, 1024, 2048, 4096};
    return kDivisors[f_ & 0x07];
  }

  uint8_t serialTxByte_ = 0;
  uint8_t serialRxByte_ = 0;
  int txCyclesRemaining_ = 0;

  // IF bit 1: latched from the RTC's TP rising edge -- see
  // pollRtc()/read()'s own OPB case and Upd1990ac's class comment. Unlike TD/RD (bits
  // 3/2), this one *is* ROM-confirmed (BASIC BEEP's repeat-gap wait polls
  // exactly this bit after commanding TP=64Hz). Also confirmed shared
  // with PB7 (see setOnKeyLine()) -- rather than a dedicated bit per
  // possible LH5811 interrupt-input pin, this looks like one shared
  // "external event" flag, with software expected to disambiguate the
  // actual cause afterward by checking each candidate pin's own live
  // level (e.g. onKeyLine_ for "was this ON/BREAK").
  static constexpr uint8_t kTpFlagBit = 0x02;
  Upd1990ac rtc_;
};

// PC-1500 memory map (see docs/pc1500_hardware_reference.md). ME0 holds
// ROM/RAM/display-buffer; ME1 holds only the I/O port controller. Keyboard
// row-sensing (IN0-IN7) is direct CPU input, not through ME0/ME1 at all --
// exposed here via readInputPort() since it's the same "what the CPU sees
// when it asks the bus" role as ME0/ME1.
class Bus : public lh5801::MemoryBus {
 public:
  // me0_ defaults to 0xFF, not 0x00: real SRAM commonly powers up with a
  // consistent non-zero bias rather than all-zero, and the system ROM's
  // MODE-key handler (gated on 79FFH != 0, confirmed via a real-hardware
  // PEEK immediately after ALL RESET + CL with fresh batteries -- no
  // code path anywhere in the ROM ever writes that address) relies on
  // this undocumented hardware behavior. See docs/pc1500_hardware_reference.md.
  //
  // The F1-F6 reserve-key area (base+8..base+0xC4, 189 bytes -- 4008H-40C4H
  // for the bare-default base of 4000H) is the one documented exception:
  // no ROM code path ever initializes it either (confirmed via instruction
  // tracing -- the assignment-lookup loop at CEC6h just scans past the end
  // into whatever garbage follows, since it never finds the 00H "end of
  // assignments" terminator the manual requires), yet real hardware reads
  // 0 there whenever no keys are assigned. That 189-byte structure must
  // already be a valid (all-zero, i.e. "nothing assigned yet")
  // 00H-terminated list before the ROM ever touches it -- there is no
  // real-hardware state equivalent to our synthetic all-0xFF default for
  // this specific range, so it's seeded to 0 explicitly here (and
  // re-seeded at its new location by reseedReserveArea()/
  // reserveAreaBase(), below, whenever a RAM-config setter moves it --
  // confirmed as a real, previously-missed bug this session: any non-bare
  // RAM config left the *live* reserve area unseeded, reproducing this
  // exact bug's ERROR 13 symptom through the ordinary Settings UI).
  explicit Bus(Keyboard& keyboard) : keyboard_(keyboard) {
    me0_.fill(0xFF);
    reseedReserveArea();  // 4008H-40C4H for the bare-default config constructed with here
    ce163Ram_.fill(0xFF);  // matches real RAM's confirmed 0xFF power-up default, not C++'s own 0
  }

  uint8_t readME0(uint16_t addr) override;
  void writeME0(uint16_t addr, uint8_t value) override;
  uint8_t readME1(uint16_t addr) override;
  void writeME1(uint16_t addr, uint8_t value) override;
  uint8_t readInputPort() override;

  // Which of the CPU's two independent memory spaces the most recent
  // readME0/writeME0/readME1/writeME1 call touched -- for display only
  // (e.g. the host UI's status panel). There's no real hardware register
  // this reads back; ME0 vs ME1 selection is purely a property of each
  // instruction's own addressing mode ((Rreg)/(ab) -> ME0, #(Rreg)/#(ab)
  // -> ME1 -- see docs/lh5801_opcode_reference.md), not a persistent CPU
  // state bit, so this is new bookkeeping rather than exposing something
  // that already existed.
  enum class MemorySpace { ME0, ME1 };
  MemorySpace lastAccessedSpace() const { return lastAccessedSpace_; }

  // Loads `size` bytes at `addr` into ME0 (e.g. a real PC-1500 ROM dump at
  // 0xC000, once the caller has one -- none is bundled here, for obvious
  // licensing reasons). Bytes beyond the target region's bounds are not
  // written.
  void loadME0(uint16_t addr, const uint8_t* data, size_t size);

  IoPortController& ioPort() { return io_; }

  // CE-150/153/158-style plug-in ROM modules at 0x8000-0xBFFF, selected by
  // the CPU's PV flip-flop (and, for modules whose ROM is bigger than the
  // 16K window, banked by PU) -- see PU/PV's own class comment in
  // lh5801.h. PV/PU only affect this region; everything else (SPU/RPU/
  // SPV/RPV's other historical uses, if any) is out of scope here.
  //
  // Four independent slots: a real machine can have a module plugged in at
  // both the 0x8000 and 0xA000 bases simultaneously, and -- since PV is a
  // single global flip-flop shared by the whole machine -- a *different*
  // module at each base for each PV level, so all four (base, PV)
  // combinations can be loaded and live at once.
  //
  // `data` is a slot's raw ROM dump, `base` is where it appears in the
  // address space, `requirePv` is the PV level the CPU must have set for
  // it to respond at all (CE-150 defaults to PV=0/low, CE-158 to PV=1/
  // high -- i.e. the two module *families* are distinguished by which PV
  // level they answer to), and `usePuBank` splits `data` into two equal
  // halves (PU=0 selects the first, PU=1 the second) for modules whose
  // real ROM exceeds the 16K window (CE-158) -- false for modules that fit
  // in one 16K bank regardless of PU (CE-150).
  //
  // A module may additionally claim a writable data-window sub-range
  // (`hasDataWindow`) -- unlike `data`, real hardware for a genuine
  // RAM-backed expansion board (as opposed to a true ROM cartridge), so
  // writes there actually stick instead of being discarded. `instructionAddr`
  // is the one address within that window where a write also triggers
  // mock command processing (see Bus::writeME0/ExpansionMock), mirroring
  // the real PC1500-PSOC5 board's DoCommand()/WriteStatus() protocol:
  // write a command byte there, poll the same address for a non-BUSY
  // status. Plain `loadrommodule`-style pure-ROM modules leave
  // `hasDataWindow` false and behave exactly as before.
  struct RomModule {
    std::vector<uint8_t> data;
    uint16_t base = 0xA000;
    bool requirePv = false;
    bool usePuBank = false;

    bool hasDataWindow = false;
    std::vector<uint8_t> dataWindow;
    uint16_t dataWindowBase = 0;
    uint16_t instructionAddr = 0;

    bool tryRead(uint16_t addr, bool pv, bool pu, uint8_t& out) const {
      if (data.empty() || pv != requirePv) return false;
      size_t bankSize = usePuBank ? data.size() / 2 : data.size();
      if (bankSize == 0 || addr < base || addr >= base + bankSize) return false;
      size_t bank = (usePuBank && pu) ? 1 : 0;
      size_t offset = bank * bankSize + (addr - base);
      if (offset >= data.size()) return false;
      out = data[offset];
      return true;
    }

    bool tryReadWindow(uint16_t addr, bool pv, uint8_t& out) const {
      if (!hasDataWindow || pv != requirePv) return false;
      if (addr < dataWindowBase || addr >= dataWindowBase + dataWindow.size()) return false;
      out = dataWindow[addr - dataWindowBase];
      return true;
    }

    // Returns true if this module's data window claims `addr` (regardless
    // of whether it's also `instructionAddr` -- Bus::writeME0 checks that
    // separately, since triggering the mock is Bus's job, not RomModule's).
    bool tryWrite(uint16_t addr, bool pv, uint8_t value) {
      if (!hasDataWindow || pv != requirePv) return false;
      if (addr < dataWindowBase || addr >= dataWindowBase + dataWindow.size()) return false;
      dataWindow[addr - dataWindowBase] = value;
      return true;
    }
  };
  static constexpr int kNumRomModules = 4;
  void loadRomModule(int slot, const uint8_t* data, size_t size, uint16_t base, bool requirePv,
                      bool usePuBank) {
    RomModule& m = romModules_[slot];
    m = RomModule{};
    m.data.assign(data, data + size);
    m.base = base;
    m.requirePv = requirePv;
    m.usePuBank = usePuBank;
  }
  // Like loadRomModule, but additionally claims a writable data-window
  // sub-range (initialized to 0xFF, matching real RAM's confirmed power-up
  // default) with command processing at `instructionAddr` -- see
  // RomModule's own comment and ExpansionMock.
  void loadExpansionModule(int slot, const uint8_t* romData, size_t romSize, uint16_t base,
                            bool requirePv, bool usePuBank, uint16_t dataWindowBase,
                            uint16_t dataWindowSize, uint16_t instructionAddr) {
    RomModule& m = romModules_[slot];
    m = RomModule{};
    m.data.assign(romData, romData + romSize);
    m.base = base;
    m.requirePv = requirePv;
    m.usePuBank = usePuBank;
    m.hasDataWindow = true;
    m.dataWindow.assign(dataWindowSize, 0xFF);
    m.dataWindowBase = dataWindowBase;
    m.instructionAddr = instructionAddr;
  }
  void unloadRomModule(int slot) { romModules_[slot] = RomModule{}; }
  bool romModuleLoaded(int slot) const { return !romModules_[slot].data.empty(); }
  const RomModule& romModule(int slot) const { return romModules_[slot]; }
  ExpansionMock& expansionMock() { return expansionMock_; }

  // Forwarded from the CPU's own SPU/RPU/SPV/RPV opcode handlers (see
  // lh5801.cpp) -- Bus is the single source of truth for the *current*
  // pin level a module ROM read sees, since the CPU has no other reason
  // to know the bus exists on this side of the interface.
  void setPv(bool v) override { pv_ = v; }
  void setPu(bool v) override { pu_ = v; }

  // PC-1500 vs PC-1500A base unit -- see docs/pc1500_hardware_reference.md's
  // "PC-1500A" section for the full derivation. The PC-1500A has 6K of
  // built-in user RAM at 4000H instead of 2K, and its 40-pin expansion
  // port is rewired (S1/S2/S3 -> S3/S4/S5) so every expansion-RAM
  // chip-select lands 1000H higher than the same physical module would on
  // a stock PC-1500. Defaults to PC1500 (stock hardware). Changing this
  // clamps extRamExtSize_ down to the new variant's own
  // extRamExtWindowMaxSize() if it's currently too big for the new
  // window (e.g. 10K selected, then switching to PC-1500A's 6K-max
  // window) -- everything else (isUnmapped/writeME0's CE-163 trigger)
  // reads the variant live via extRamExtBase(), so there's no other
  // "recompute on change" step needed. Only takes effect at the next
  // reset/cold-start, same as the extension-RAM sizes below -- the ROM's
  // own boot-time scan is what actually discovers how much RAM is
  // installed; this class does no detection of its own.
  enum class MachineVariant { PC1500, PC1500A };
  void setMachineVariant(MachineVariant v) {
    machineVariant_ = v;
    // Not std::min -- some translation units (state_roundtrip_test.cpp)
    // include <windows.h> without NOMINMAX first, whose own min/max
    // macros silently mangle std::min's own name at the call site.
    size_t maxSize = extRamExtWindowMaxSize();
    if (extRamExtSize_ > maxSize) extRamExtSize_ = maxSize;
    // No reseedReserveArea() call here, deliberately -- reserveAreaBase()
    // doesn't depend on machineVariant_ at all (PC-1500 vs PC-1500A only
    // differs in how far built-in RAM extends *above* 4000H, never in
    // what's contiguous *below* it), so this setter can't move it.
  }
  MachineVariant machineVariant() const { return machineVariant_; }

  // Emulated module RAM, two independent regions/windows -- see
  // docs/pc1500_hardware_reference.md's memory map. Both off (size 0) by
  // default, matching stock hardware with no module installed. Size is in
  // bytes, backed starting at the window's low address; the rest of the
  // window (if size is less than the window's full span) stays unmapped,
  // matching a physically smaller module not filling its whole socket.
  // Changing the size doesn't clear underlying bytes, so shrinking and
  // growing back preserves whatever was there.
  //
  // Expansion window (2K module RAM slot): 4800H-6FFFH on a PC-1500 (up
  // to 10K span; real 1982 hardware options were 4K or 8K here, CE-151
  // and an unreleased 8K variant respectively -- 10K, the window's full
  // physical span, wasn't a real period-correct module but is easy to
  // emulate and physically possible with denser modern RAM), or
  // 5800H-6FFFH on a PC-1500A (up to 6K span only -- the PC-1500A's extra
  // 4K of built-in RAM already absorbs what would otherwise be the
  // window's own first 4K, and its upper bound stays 6FFFH either way
  // since 7000H+ is fixed onboard hardware the expansion-port rewiring
  // doesn't touch). See extRamExtBase()/extRamExtWindowMaxSize().
  //
  // Mutually exclusive with CE-163, CE-155, and CE-168N (see their own
  // setters below) -- a real PC-1500(A) has one expansion port, so at most
  // one of these can be physically installed at a time.
  void setExtRamExtSize(size_t bytes) {
    extRamExtSize_ = bytes;
    if (bytes != 0) {
      ce163Enabled_ = false;
      ce155Enabled_ = false;
      ce168nEnabled_ = false;
    }
  }
  size_t extRamExtSize() const { return extRamExtSize_; }
  // Base address of the expansion window for the current machine variant
  // -- 4800H (PC-1500) or 5800H (PC-1500A). Public so callers (the
  // Settings menu, tests) can label/verify addresses without duplicating
  // the variant check.
  uint16_t extRamExtBase() const {
    return machineVariant_ == MachineVariant::PC1500A ? 0x5800 : 0x4800;
  }
  // Full physical span of the expansion window for the current variant --
  // 10K (PC-1500) or 6K (PC-1500A). Not a static constexpr (unlike
  // kExtRam0000WindowSize below) since it depends on machineVariant_.
  size_t extRamExtWindowMaxSize() const { return 0x7000 - extRamExtBase(); }

  // 0000H-3FFFH window (option user memory slot, up to 16K span): not a
  // real 1982-era option at all (nothing plugged in there back then), but
  // physically possible now and worth emulating for headroom. Unaffected
  // by machine variant -- the PC-1500A's expansion-port rewiring only
  // touches the S1/S2/S3 signals the *other* window's chip-selects are
  // built from, not this one.
  //
  // Mutually exclusive with CE-163, CE-155, and CE-168N (see their own
  // setters below) -- all four occupy this exact same window.
  static constexpr size_t kExtRam0000WindowSize = 0x4000;  // 16K
  void setExtRam0000Size(size_t bytes) {
    extRam0000Size_ = bytes;
    // Unconditional, unlike setExtRamExtSize's own clearing above: None,
    // 16K, CE-163, CE-155, and CE-168N are five mutually-exclusive
    // alternatives *within this one window/submenu*, so selecting "None"
    // (bytes == 0) must deactivate the others too, not just leave a nonzero
    // size in place. Before this fix, bytes == 0 skipped the clear entirely,
    // so selecting "None" while CE-163 was enabled did nothing -- the window
    // stayed banked, since isUnmapped()'s 0000H-window check gates on
    // !ce163Enabled_ regardless of extRam0000Size_.
    ce163Enabled_ = false;
    ce155Enabled_ = false;
    ce168nEnabled_ = false;
    reseedReserveArea();  // may move reserveAreaBase() -- see its own comment
  }
  size_t extRam0000Size() const { return extRam0000Size_; }

  // CE-163: a real 1982-era module, 32K of RAM banked into the 0000H-3FFFH
  // window two 16K banks at a time (only one bank visible at once). Bank
  // selection is a pure address-line trigger on real hardware: any WRITE
  // to a 2K range selects bank 0 (even address) or bank 1 (odd address)
  // -- the byte value written is irrelevant, and nothing is actually
  // stored there (see Bus::writeME0). That trigger range is itself
  // machine-variant-dependent, same underlying reason as the expansion
  // window above: 5800H-5FFFH on a PC-1500 (pin 18 = S3), or
  // 6800H-6FFFH on a PC-1500A (same physical pin, now wired to S5).
  //
  // Mutually exclusive with both extension-RAM windows above, CE-155, and
  // CE-168N below (enabling this clears all four; any of them going active
  // disables this) -- real hardware has one expansion port, so at most
  // one of these five can be physically installed at a time.
  void setCe163Enabled(bool enabled) {
    ce163Enabled_ = enabled;
    if (enabled) {
      extRam0000Size_ = 0;
      extRamExtSize_ = 0;
      ce155Enabled_ = false;
      ce168nEnabled_ = false;
    }
    reseedReserveArea();  // may move reserveAreaBase() -- see its own comment
  }
  bool ce163Enabled() const { return ce163Enabled_; }
  uint8_t ce163Bank() const { return ce163Bank_; }

  // CE-155: a real 1982-era 8K module, split across both windows above --
  // 2K at exactly 3800H-3FFFH (the *top* of the 0000H-3FFFH window, not
  // its base -- real hardware isolates just this 2K, unlike the generic
  // 0000H-window size above which is always left-aligned from 0000H) plus
  // 6K filling the entire expansion window (4800H-5FFFH on a PC-1500,
  // 5800H-6FFFH on a PC-1500A). Confirmed live this session: this genuine
  // 3800H isolation is what lets BASIC's own boot-time RAM scan compute
  // the real CE-155 program-start origin (38C5H) instead of the stock
  // unit's 40C5H -- see docs/pc1500_hardware_reference.md.
  //
  // Mutually exclusive with both extension-RAM windows, CE-163, and
  // CE-168N (see their own setters) -- same one-expansion-port reasoning.
  void setCe155Enabled(bool enabled) {
    ce155Enabled_ = enabled;
    if (enabled) {
      extRam0000Size_ = 0;
      extRamExtSize_ = 0;
      ce163Enabled_ = false;
      ce168nEnabled_ = false;
    }
    reseedReserveArea();  // may move reserveAreaBase() -- see its own comment
  }
  bool ce155Enabled() const { return ce155Enabled_; }

  // CE-168N: a generalized version of the CE-163 hack above, for a
  // parametrized flash/RAM module rather than a fixed, hardware-accurate
  // one. Same window (0000H-3FFFH), same fixed 16K-per-bank size, same
  // machine-variant-dependent bank-select trigger range as CE-163
  // (5800H-5FFFH on a PC-1500, 6800H-6FFFH on a PC-1500A) -- neither the
  // load address nor the per-bank size is configurable, only these two:
  //
  // `banks`: total number of 16K banks (0 disables the module, same "0 =
  // off" convention as setExtRam0000Size/setExtRamExtSize). Bank select is
  // `addr % banks` (not `addr & mask`, since banks isn't guaranteed to be a
  // power of two -- CE-163's real hardware only ever wires one address
  // line for its fixed 2 banks; this module is explicitly non-hardware, so
  // there's no equivalent constraint to preserve).
  //
  // `firstRoBank`: banks at or above this index simulate a flash chip --
  // CPU writes (via writeME0, including the `poke` FIFO command, which
  // intentionally mimics a CPU write) to a read-only bank are silently
  // discarded, matching flash ignoring a write pulse rather than erroring.
  // A read-only bank is still fully writable through loadME0 (the
  // `loadbinary` FIFO command's underlying path) -- that's how a preset
  // seeds initial flash content before boot, as opposed to a running ROM
  // attempting to reprogram it live. Pass firstRoBank >= banks for "every
  // bank writable".
  //
  // Mutually exclusive with CE-163, CE-155, and both extension-RAM windows
  // above -- same one-expansion-port reasoning as setCe163Enabled.
  void setCe168nEnabled(uint8_t banks, uint8_t firstRoBank) {
    ce168nEnabled_ = banks != 0;
    ce168nBanks_ = banks;
    ce168nFirstRoBank_ = firstRoBank;
    ce168nBank_ = 0;
    ce168nRam_.assign(static_cast<size_t>(banks) * 0x4000, 0xFF);  // matches
                                                                    // real RAM's
                                                                    // confirmed
                                                                    // 0xFF
                                                                    // power-up
                                                                    // default
    if (ce168nEnabled_) {
      extRam0000Size_ = 0;
      extRamExtSize_ = 0;
      ce163Enabled_ = false;
      ce155Enabled_ = false;
    }
    reseedReserveArea();  // may move reserveAreaBase() -- see its own comment
  }
  bool ce168nEnabled() const { return ce168nEnabled_; }
  uint8_t ce168nBank() const { return ce168nBank_; }

  // Wraps Keyboard::setKeyState. The actual release is deferred by
  // kMinimumHoldCycles (see applyRelease) rather than applied immediately,
  // so a host keypress briefer than one ROM timer-interrupt period can't
  // go all the way down and back up between two keyboard scans without
  // any scan ever seeing it.
  //
  // History, worth keeping in mind if key responsiveness ever looks wrong
  // again: this used to *also* force-clear the ROM's key-dispatch gate
  // (RAM flag at 7B0EH bit 0) the instant a non-cursor release left
  // nothing held, because that flag's own ~8-timer-period software
  // countdown didn't match observed real-hardware responsiveness (typing
  // several keys a second with no perceptible delay). That was
  // compensating for a real bug, not a genuine hardware quirk: the CPU
  // timer was modeled as a linear counter instead of the real 9-bit
  // *polynomial* one (see lh5801_timer_table.h; fixed 2026-08-02). With
  // the correct polynomial sequence, the ROM's own countdown clears the
  // gate on its own within a few thousand cycles -- the force-clear was
  // removed once this was confirmed, and confirmed *necessary* to remove:
  // it had been silently breaking BASWORD's own keyboard-hook timing
  // (see [[pc1500_keyword_table_mechanism]] in memory for the full story
  // -- `BASWORD +"name";"..."` only started actually registering new
  // keywords once this force-clear was gone).
  void setKeyState(Key key, bool pressed);

  // Debug-only: exposes cursor-key repeat-timing state directly, for
  // testing the double-press fix without the confound of normal ROM
  // execution also touching 7B0EH during a `run`.
  bool debugCursorKeyHeld() const { return cursorKeyHeld_; }
  int debugCursorRepeatCycles() const { return cursorRepeatCycles_; }
  bool debugCursorRepeatFired() const { return cursorRepeatFired_; }

  // Drives cursor-key rollover and deferred key releases (see
  // setKeyState): call once per emulated cycle batch (see main.cpp's frame
  // loop) with the number of cycles just executed.
  //
  // Rollover: while a cursor key is held, the ROM's dispatch never re-fires
  // on its own (traced directly: holding Left for 1.5M cycles touches
  // 7B0EH exactly once, at the initial press), so this clears bit 0
  // periodically at the ~5/sec rate confirmed on real hardware, letting
  // the ROM's own dispatch logic re-trigger a repeat.
  void advanceCycles(int cycles);

  // Forwards to the I/O port controller's RTC -- see
  // IoPortController::pollRtc(). Harmless to call as often or as rarely as
  // convenient (see its own comment); main.cpp's frame loop calls this
  // once per rendered frame mainly so IF bit 1 stays reasonably current
  // even during stretches where nothing happens to read OPB directly.
  bool pollRtc() { return io_.pollRtc(); }

  // Session state save/load -- narrow scope: RAM contents
  // (me0_[0x0000,0x8000) only -- covers both extension-RAM windows, the
  // built-in 2K-or-6K RAM, and the LCD buffer/system RAM/their mirrors,
  // all backed by the same array), extension-RAM window sizes, the
  // machine variant and CE-155 flag (version 5+), and all four ROM
  // module slots (raw bytes + base/requirePv/usePuBank -- module ROM isn't
  // stored in me0_ at all, see RomModule::tryRead). Deliberately excludes
  // 0x8000H-0xFFFFH (module-ROM range isn't backed by me0_; the base
  // system ROM at 0xC000H-0xFFFFH is always reloaded fresh from the
  // command-line/conf-file ROM path, not duplicated into the state file)
  // and IoPortController/RTC state (left to the ROM's own boot-time
  // reinitialization). CPU registers are saved separately (CPU::saveState/
  // loadState) and composed with this at the src/hoststate/state_file.h
  // level, which also holds the file-level magic/version header -- see
  // that header's comment for why restoring CPU state (and *not* calling
  // cpu.reset() afterward) is the correct model of a real PC-1500's OFF/ON
  // cycle, not a reset.
  void saveState(std::ostream& os) const;

  // The RAM-config scalars a state file carries, surfaced to a caller when
  // loadState rejects a load over a mismatch -- so it can offer "apply
  // this configuration and try again" instead of just reporting an error
  // and discarding the saved session outright.
  struct SavedConfig {
    size_t extRamExtSize = 0;
    size_t extRam0000Size = 0;
    bool ce163Enabled = false;
    MachineVariant machineVariant = MachineVariant::PC1500;
    bool ce155Enabled = false;
  };

  // Refuses to load (returns false, leaving `this` completely untouched --
  // not even partially applied) if the saved RAM-config scalars
  // (extRamExtSize/extRam0000Size/ce163Enabled/machineVariant/ce155Enabled)
  // don't match this Bus's *current* values for those exact same fields.
  // Deliberate: raw me0_ contents are only meaningful relative to the
  // specific memory-map shape they were saved under (program area,
  // reserve-key area, etc. all live at different addresses depending on
  // installed RAM) -- loading them into a differently-configured Bus is
  // the same category of nonsensical as real PC-1500 RAM (short of a
  // battery-backed module) simply not surviving a hardware
  // reconfiguration. The caller is responsible for applying the intended
  // config (setMachineVariant/setExtRamExtSize/setExtRam0000Size/
  // setCe163Enabled/setCe155Enabled) *before* calling this -- see
  // main.cpp's startup sequence, which now applies AppConfig's persisted
  // settings unconditionally, before attempting any state-file load, so
  // there's always a well-defined "current config" to compare against.
  // `error`, if non-null, is set to a human-readable reason on failure
  // (config mismatch, or truncated/corrupt data). `configMismatch` and
  // `savedConfig`, if non-null, are set/populated only on the config-
  // mismatch failure path (left untouched -- not even zeroed -- on
  // success or on any other failure) -- see main.cpp's startup-time
  // mismatch prompt, the only current caller that reads them.
  bool loadState(std::istream& is, std::string* error = nullptr, bool* configMismatch = nullptr,
                 SavedConfig* savedConfig = nullptr);

 private:
  // Applies a release that setKeyState deferred: updates Keyboard, and (for
  // non-cursor keys) the same 7B0EH-bit-0 clear setKeyState's comment
  // describes, now that the release is actually taking effect.
  void applyRelease(Key key);


  static bool isRom(uint16_t addr) { return addr >= 0xC000; }
  // When ce163Enabled_/ce168nEnabled_, the 0000H window is always "mapped"
  // here regardless of extRam0000Size_ (which is forced to 0 by the mutual
  // exclusion in setCe163Enabled/setCe168nEnabled) -- both have their own
  // separate backing store (ce163Ram_/ce168nRam_), read/written directly in
  // readME0/writeME0, not gated by this size check at all.
  //
  // ce155Enabled_ short-circuits both windows to CE-155's own fixed,
  // real topology instead of the generic size-gated checks: the 0000H
  // window is mapped ONLY at its top 2K (3800H-3FFFH, unlike the generic
  // left-aligned-from-0000H check below), and the expansion window is
  // mapped for exactly 6K from its (variant-dependent) base -- see
  // setCe155Enabled's own comment.
  bool isUnmapped(uint16_t addr) const {
    bool in0000Window = addr <= 0x3FFF;
    uint16_t extBase = extRamExtBase();
    bool inExtWindow = addr >= extBase && addr <= 0x6FFF;
    if (ce155Enabled_) {
      if (in0000Window) return !(addr >= 0x3800 && addr <= 0x3FFF);
      if (inExtWindow) return (addr - extBase) >= 0x1800;  // 6K
      return addr >= 0x8000 && addr <= 0xBFFF;              // CE-150/153/158 (not connected)
    }
    return (in0000Window && addr >= extRam0000Size_ && !ce163Enabled_ &&
             !ce168nEnabled_) ||  // beyond configured 0000H module RAM
           (inExtWindow && (addr - extBase) >= extRamExtSize_) ||  // beyond configured expansion RAM
           (addr >= 0x8000 && addr <= 0xBFFF);                // CE-150/153/158 (not connected)
  }
  // 7C00H-7FFFH on a base PC-1500 is a duplicate of 7800H-7BFFH (the 1K
  // system RAM) -- a half-decoded chip-select block, confirmed directly on
  // real hardware. On a PC-1500A, this range is instead a real, independent
  // 1K of its own -- confirmed by the user; not yet independently verified
  // on real PC-1500A hardware the way the base-unit mirror was, but treated
  // as authoritative. The base PC-1500's own behavior here is otherwise
  // undefined/unspecified by any source this project has -- it's simply
  // left exactly as it already was (this mirror), rather than guessed at
  // further, since that's already the confirmed, working behavior.
  //
  // 7000H-77FFH is driven by a RAM chip smaller than its address window:
  // address bits 9 and 10 (0200H/0400H) simply aren't decoded, so every
  // address in this range aliases the one with those two bits forced high
  // (i.e. ORed with 0600H). Confirmed directly on real hardware for
  // 7000H<->7600H, 7100H<->7700H, and 7400H<->7600H, and for 7400H NOT
  // aliasing 7A00H (bit 11 differs, outside this window). This supersedes
  // an earlier, narrower model (7000H-71FFH only, based on the PC-2 memory
  // map in TRS-80 Microcomputer News, March 1983, p.26, which called the
  // whole 7000H-75FFH range a duplicate of 7600H-7BFFH -- closer to
  // correct than our first, narrower correction, just off on the exact
  // mechanism). 4000H-47FFH (the 2K user RAM) is NOT mirrored into
  // 4800H-4FFFH. This 7000H-77FFH aliasing applies to both machine
  // variants alike -- only the 7C00H-7FFFH case differs by variant.
  static uint16_t effectiveAddr(uint16_t addr, MachineVariant variant) {
    if (addr >= 0x7000 && addr <= 0x77FF) return static_cast<uint16_t>(addr | 0x0600);
    if (variant == MachineVariant::PC1500 && addr >= 0x7C00 && addr <= 0x7FFF) {
      return static_cast<uint16_t>(addr - 0x0400);
    }
    return addr;
  }

  // Base of the contiguous free-RAM region leading into 4000H -- the same
  // "base" the ROM's own boot-time RAM detection settles on, and that
  // basicProgramStart()'s live 7865H/7866H pointer reflects (confirmed
  // this session: 0x0000 with a full 0000H-window module or CE-163,
  // 0x3800 with CE-155, else 0x4000 bare) -- and therefore also where the
  // F1-F6 "reserve area" (base+8 .. base+0xC4, see reseedReserveArea's
  // own comment) actually lives.
  uint16_t reserveAreaBase() const {
    if (ce163Enabled_ || extRam0000Size_ >= kExtRam0000WindowSize) return 0x0000;
    if (ce155Enabled_) return 0x3800;
    return 0x4000;
  }

  // The F1-F6 reserve-key area is 189 bytes, base+8..base+0xC4 inclusive
  // (see the constructor's own comment for why it must be pre-zeroed).
  // Its location moves with reserveAreaBase() whenever installed RAM
  // changes, so this needs to be re-applied any time a setter changes
  // that -- not just once at construction -- *and* after loadState()
  // restores a saved me0_ snapshot (see that function's own call site):
  // a state saved before this method existed (or under a different RAM
  // config than the one it's now being loaded into) can have its live
  // reserve area sitting at the generic, never-seeded 0xFF fill, exactly
  // reproducing the original ERROR 13 bug even on a build that has this
  // fix -- confirmed live this session, a real gap in the first version
  // of this fix (which only covered the constructor and the four
  // RAM-config setters, not a *loaded* config).
  //
  // Only reseeds a range that's still entirely the generic 0xFF fill --
  // i.e. provably never touched since power-on/load, matching this
  // area's own "there is no real-hardware equivalent of a truly
  // uninitialized reserve area" invariant. Deliberately NOT unconditional:
  // a range holding real F1-F6 assignment data (or already-correctly-
  // zeroed empty data) never reads back as all-0xFF, so this can't
  // clobber genuine saved assignments -- only ever fixes a location this
  // invariant has never actually been established for yet.
  void reseedReserveArea() {
    uint16_t base = reserveAreaBase();
    bool stillGenericFill = std::all_of(me0_.begin() + base + 0x8, me0_.begin() + base + 0xC5,
                                         [](uint8_t b) { return b == 0xFF; });
    if (stillGenericFill) {
      std::fill(me0_.begin() + base + 0x8, me0_.begin() + base + 0xC5, 0x00);
    }
  }

  std::array<uint8_t, 65536> me0_{};
  IoPortController io_;
  Keyboard& keyboard_;
  MemorySpace lastAccessedSpace_ = MemorySpace::ME0;
  MachineVariant machineVariant_ = MachineVariant::PC1500;
  size_t extRamExtSize_ = 0;
  size_t extRam0000Size_ = 0;
  bool ce163Enabled_ = false;
  bool ce155Enabled_ = false;
  uint8_t ce163Bank_ = 0;
  std::array<uint8_t, 0x8000> ce163Ram_{};  // 32K, two independent 16K banks back-to-back
  bool ce168nEnabled_ = false;
  uint8_t ce168nBank_ = 0;
  uint8_t ce168nBanks_ = 0;
  uint8_t ce168nFirstRoBank_ = 0;
  std::vector<uint8_t> ce168nRam_;  // ce168nBanks_ independent 16K banks back-to-back
  std::array<RomModule, kNumRomModules> romModules_;
  ExpansionMock expansionMock_;
  bool pv_ = false;  // matches CPU::reset()'s own pv_/pu_ default
  bool pu_ = false;

  // Cursor-key rollover state (see advanceCycles).
  bool cursorKeyHeld_ = false;
  Key heldCursorKey_ = Key::Left;
  int cursorRepeatCycles_ = 0;
  // Whether this hold has already fired its first repeat -- real keyboard
  // auto-repeat has two distinct timings (a longer initial delay, then a
  // faster ongoing rate once repeating has started); this distinguishes
  // which threshold (kCursorInitialDelayCycles vs kCursorRepeatCycles)
  // applies. Reset alongside cursorRepeatCycles_ on every fresh press.
  bool cursorRepeatFired_ = false;
  // ~1s at 1.3MHz: Paul's own hardware observation ("close to one second
  // held down to start repeating") -- confirmed distinct from the ongoing
  // repeat rate below, which an earlier tuning pass had wrongly applied
  // to the initial delay too (making the first repeat fire ~6x too soon).
  static constexpr int kCursorInitialDelayCycles = 1300000;
  // ~154ms at 1.3MHz: Paul's own "~5/sec" estimate, refined after
  // side-by-side comparison to hardware (initial 200ms guess ran ~30%
  // slower than the real repeat rate). This is the *ongoing* rate once
  // repeating has already started -- see kCursorInitialDelayCycles above
  // for the (much longer) delay before the first repeat.
  static constexpr int kCursorRepeatCycles = 200000;

  // Deferred-release state (see setKeyState/advanceCycles/applyRelease).
  struct PendingRelease {
    Key key;
    int cyclesRemaining;
  };
  std::vector<PendingRelease> pendingReleases_;
  // Comfortably more than one ~32768-cycle timer period (512 ticks x 64
  // cycles/tick), so a release is never applied before at least one full
  // keyboard-scan period has had a chance to see the key down.
  static constexpr int kMinimumHoldCycles = 40000;
};

}  // namespace pc1500
