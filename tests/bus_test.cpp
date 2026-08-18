#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "bus.h"
#include "keyboard.h"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

// uPD1990AC RTC test helpers -- see Upd1990ac in bus.h. Command bits are
// latched on PC1 (STB, 0x02) rising edges with PC3-5 (0x08/0x10/0x20)
// holding C0/C1/C2; data is clocked on PC2 (CLK, 0x04) rising edges with
// PC0 (0x01) holding the bit. All three sequences below write the
// low-6-bits value twice (once at the old level, once at the new) purely
// so a human reading a trace sees an explicit "before/after" pair --
// IoPortController only reacts to the transition itself.
void rtcLatchCommand(pc1500::IoPortController& io, bool c0, bool c1, bool c2) {
  uint8_t levels = static_cast<uint8_t>((c0 ? 0x08 : 0) | (c1 ? 0x10 : 0) | (c2 ? 0x20 : 0));
  io.write(0x08, levels);
  io.write(0x08, static_cast<uint8_t>(levels | 0x02));  // STB rising edge
  io.write(0x08, levels);
}

// Feeds `value`'s 40 low bits in LSB-first, i.e. bit i is fed on the i-th
// clock. Since the shift register shifts right with new bits entering at
// bit 39, the last bit fed (value's bit 39) ends up at bit 39 and the
// first bit fed (value's bit 0) ends up at bit 0 after all 40 clocks --
// i.e. the register ends up equal to `value` itself. Must be called with
// Register Shift or Time Set already latched (see rtcLatchCommand).
void rtcShiftIn(pc1500::IoPortController& io, uint64_t value) {
  for (int i = 0; i < 40; i++) {
    uint8_t dataBit = static_cast<uint8_t>((value >> i) & 1);
    io.write(0x08, dataBit);
    io.write(0x08, static_cast<uint8_t>(dataBit | 0x04));  // CLK rising edge
  }
}

// Inverse of rtcShiftIn: samples DATA OUT (OPB bit 6) before each of 40
// clocks, so bit i of the result is the register's bit i at the time it
// was sampled -- reconstructing the pre-shift register value bit-for-bit.
uint64_t rtcShiftOut(pc1500::IoPortController& io) {
  uint64_t value = 0;
  for (int i = 0; i < 40; i++) {
    if (io.read(0x0F) & 0x40) value |= (static_cast<uint64_t>(1) << i);
    io.write(0x08, 0x04);  // CLK rising edge (DATA IN doesn't matter here)
    io.write(0x08, 0x00);
  }
  return value;
}

uint64_t rtcPackBcd40(int month, int dow, int day, int hour, int minute, int second) {
  auto bcdNibbles = [](int v) { return std::make_pair(v / 10, v % 10); };
  auto [dayTens, dayUnits] = bcdNibbles(day);
  auto [hourTens, hourUnits] = bcdNibbles(hour);
  auto [minTens, minUnits] = bcdNibbles(minute);
  auto [secTens, secUnits] = bcdNibbles(second);
  uint64_t reg = 0;
  reg |= static_cast<uint64_t>(secUnits) << 0;
  reg |= static_cast<uint64_t>(secTens) << 4;
  reg |= static_cast<uint64_t>(minUnits) << 8;
  reg |= static_cast<uint64_t>(minTens) << 12;
  reg |= static_cast<uint64_t>(hourUnits) << 16;
  reg |= static_cast<uint64_t>(hourTens) << 20;
  reg |= static_cast<uint64_t>(dayUnits) << 24;
  reg |= static_cast<uint64_t>(dayTens) << 28;
  reg |= static_cast<uint64_t>(dow) << 32;
  reg |= static_cast<uint64_t>(month) << 36;
  return reg;
}

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                        \
      std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

void testUnmappedRegionsReadHighAndIgnoreWrites() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  // Option user memory (no module installed).
  CHECK(bus.readME0(0x0000) == 0xFF);
  bus.writeME0(0x0000, 0x42);
  CHECK(bus.readME0(0x0000) == 0xFF);  // write ignored

  // CE-150/153/158 region, not connected.
  CHECK(bus.readME0(0x9000) == 0xFF);
}

void testMirroredRegionsAliasRealRam() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  // 7C00H-7FFFH is a duplicate of 7800H-7BFFH (the 1K system RAM),
  // confirmed directly on real hardware.
  bus.writeME0(0x7C00, 0x33);
  CHECK(bus.readME0(0x7800) == 0x33);
  bus.writeME0(0x7FFF, 0x44);
  CHECK(bus.readME0(0x7BFF) == 0x44);

  // 7000H-77FFH is driven by a RAM chip smaller than its address window:
  // bits 9/10 (0200H/0400H) aren't decoded, so every address aliases the
  // one with those bits forced high (addr | 0600H). 7000H<->7600H,
  // 7100H<->7700H, 7200H<->7600H, and 7400H<->7600H are each confirmed
  // directly on real hardware.
  bus.writeME0(0x7000, 0x11);
  CHECK(bus.readME0(0x7600) == 0x11);
  bus.writeME0(0x71FF, 0x22);
  CHECK(bus.readME0(0x77FF) == 0x22);
  bus.writeME0(0x7400, 0x33);
  CHECK(bus.readME0(0x7600) == 0x33);
  bus.writeME0(0x7200, 0x55);
  CHECK(bus.readME0(0x7600) == 0x55);

  bus.writeME0(0x7A00, 0x00);
  bus.writeME0(0x7400, 0x77);
  CHECK(bus.readME0(0x7A00) == 0x00);  // unaffected -- outside this chip-select block
}

// Unlike the base PC-1500 (mirrored into 7800H-7BFFH, see
// testMirroredRegionsAliasRealRam above -- that behavior must stay exactly
// as-is), the PC-1500A has real, independent 1K RAM at 7C00H-7FFFH.
void testPC1500A_7C00RangeIsRealRam() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500A);

  bus.writeME0(0x7C00, 0x33);
  bus.writeME0(0x7800, 0x99);
  CHECK(bus.readME0(0x7C00) == 0x33);  // not aliased to 7800H's own value
  CHECK(bus.readME0(0x7800) == 0x99);

  bus.writeME0(0x7FFF, 0x44);
  bus.writeME0(0x7BFF, 0x88);
  CHECK(bus.readME0(0x7FFF) == 0x44);  // not aliased to 7BFFH's own value
  CHECK(bus.readME0(0x7BFF) == 0x88);
}

// The F1-F6 "reserve area" must read back all-zero (real hardware never
// sees a truly uninitialized version of it -- see Bus's constructor's own
// comment), and that's true regardless of which base it lives at for the
// current RAM config, not just the bare-default 4008H-40C4H case.
void testReserveAreaIsZeroedAtDefaultBase() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  for (uint16_t addr = 0x4008; addr <= 0x40C4; addr++) {
    CHECK(bus.readME0(addr) == 0x00);
  }
  // Just outside the reserve area on both sides: still the generic 0xFF
  // fill, confirming the zeroing is scoped to exactly this range.
  CHECK(bus.readME0(0x4007) == 0xFF);
  CHECK(bus.readME0(0x40C5) == 0xFF);
}

// Enabling 16K at 0000H shifts basicProgramStart() (and therefore the
// reserve area, which always sits immediately before it) down to 0000H --
// confirmed live this session. Before this fix, the reserve area's zero
// seeding was a one-time, hardcoded 4008H-40C4H applied only at
// construction, so this config left the *live* reserve area (0008H-00C4H)
// unseeded, reproducing the original "RESERVE key throws ERROR 13"
// bug's exact precondition through the ordinary Settings UI.
void testReserveAreaFollowsExtRam0000Config() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);  // 16K, fills the whole window
  for (uint16_t addr = 0x0008; addr <= 0x00C4; addr++) {
    CHECK(bus.readME0(addr) == 0x00);
  }
}

void testRomIsReadOnly() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME0(0xC000, 0x99);
  CHECK(bus.readME0(0xC000) == 0xFF);  // write to ROM region ignored (0xFF is the unwritten default)

  uint8_t data[] = {0x11, 0x22, 0x33};
  bus.loadME0(0xC000, data, sizeof(data));
  CHECK(bus.readME0(0xC000) == 0x11);
  CHECK(bus.readME0(0xC001) == 0x22);
  CHECK(bus.readME0(0xC002) == 0x33);
}

void testRamRegionsAreReadWrite() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME0(0x4000, 0xAB);  // standard user RAM
  CHECK(bus.readME0(0x4000) == 0xAB);
  bus.writeME0(0x7600, 0xCD);  // display buffer, chips 1&3
  CHECK(bus.readME0(0x7600) == 0xCD);
  bus.writeME0(0x7800, 0xEF);  // system RAM
  CHECK(bus.readME0(0x7800) == 0xEF);
}

void testIoPortControllerDdaGatesOpaReadback() {
  pc1500::IoPortController io;
  io.write(0x0C, 0xF0);  // DDA: high nibble output, low nibble input
  io.write(0x0E, 0xAB);  // OPA = 0xAB
  // Output bits (high nibble) reflect opa_; input bits (low nibble) read
  // back as 1 (undriven/pulled-up).
  CHECK(io.opaOutput() == 0xAF);
  CHECK(io.read(0x0E) == 0xAB);  // raw register readback is unaffected by DDA
}

void testOpcBit6ControlsBuzzerOn() {
  // PC6 (OPC bit 6) is the buzzer on/off control -- see
  // docs/pc1500_hardware_reference.md's I/O-PC pin table. buzzerOn()
  // is what main.cpp's audio-sample generation reads every cycle
  // (real BASIC BEEP works by the ROM toggling this bit at audio rate
  // in a timed software loop), so it must track OPC bit 6 exactly, in
  // both directions and independent of the register's other bits.
  pc1500::IoPortController io;
  CHECK(!io.buzzerOn());
  io.write(0x08, 0x40);
  CHECK(io.buzzerOn());
  io.write(0x08, 0xFF);
  CHECK(io.buzzerOn());
  io.write(0x08, 0xBF);  // every bit except PC6
  CHECK(!io.buzzerOn());
  io.write(0x08, 0x00);
  CHECK(!io.buzzerOn());
}

void testSerialTransmitSetsTdFlagAfterProgrammedDelay() {
  // Confirmed via CE-150 (printer/cassette interface) ROM disassembly:
  // it polls IF bit 3 (0x08) clear before writing the next byte to
  // register 0x06, i.e. writing 0x06 starts a transmission that clears
  // TD, and TD sets again once the transmission (start + 8 data + 2 stop
  // bits, at the rate F's low 3 bits select) finishes.
  pc1500::IoPortController io;
  io.write(0x07, 0x00);  // F divisor bits = 0 -> fastest rate (1 cycle/bit)
  io.write(0x06, 0xA5);  // start a transmission
  CHECK((io.read(0x0B) & 0x08) == 0x00);  // TD clear while busy
  io.advanceCycles(10);
  CHECK((io.read(0x0B) & 0x08) == 0x00);  // not done yet (11 bit periods needed)
  io.advanceCycles(1);
  CHECK((io.read(0x0B) & 0x08) == 0x08);  // TD set once the 11th bit period elapses
}

void testSerialTransmitDelayScalesWithClockDivisor() {
  pc1500::IoPortController io;
  io.write(0x07, 0x01);  // F divisor bits = 1 -> divisor 2 -> 22 cycles total
  io.write(0x06, 0x00);
  io.advanceCycles(21);
  CHECK((io.read(0x0B) & 0x08) == 0x00);
  io.advanceCycles(1);
  CHECK((io.read(0x0B) & 0x08) == 0x08);
}

void testSerialTransmitRegisterIsWriteOnly() {
  // Per the Service Manual's register table, 0x06 has no read column
  // (it's a trigger, not readable storage).
  pc1500::IoPortController io;
  io.write(0x06, 0x42);
  CHECK(io.read(0x06) == 0xFF);
}

void testSerialReceiveReadClearsRdFlag() {
  // U (register 0x05) is documented as "write: n/a" / "read: contents of
  // U, resets RD flag" -- no ROM code path exercising real reception has
  // been traced, so this only checks the read-clears-the-flag mechanics,
  // not that anything ever sets RD in the first place.
  pc1500::IoPortController io;
  io.write(0x0B, 0x04);  // force IF bit 2 (RD, per our best-guess bit assignment) set
  CHECK((io.read(0x0B) & 0x04) == 0x04);
  io.read(0x05);
  CHECK((io.read(0x0B) & 0x04) == 0x00);
  io.write(0x05, 0x99);  // write is documented as a no-op
  CHECK(io.read(0x05) == 0x00);
}

void testRtcTpRateSelectAndIfBitLatch() {
  // Confirmed via ROM1.BIN disassembly (E890-E8B4): BASIC BEEP's
  // repeat-gap wait issues this exact TP=64Hz command (C2=1,C1=0,C0=0)
  // before polling PB5/IF bit 1 -- see Upd1990ac's class comment. Default
  // rate is already 64Hz, so this test explicitly selects 256Hz instead,
  // to prove the command genuinely changes behavior rather than
  // coincidentally matching the default.
  pc1500::IoPortController io;
  io.useManualRtcClock();  // see its own comment -- must precede latchCommand
  rtcLatchCommand(io, /*c0=*/true, /*c1=*/false, /*c2=*/true);  // TP = 256 Hz

  CHECK((io.read(0x0B) & 0x02) == 0x00);  // IF bit 1 clear initially
  CHECK((io.read(0x0F) & 0x20) == 0x00);  // PB5 (TP) starts low

  double halfPeriod = 0.5 / 256.0;
  io.advanceManualRtcClock(halfPeriod * 1.01);  // just over one half-period -> one rising edge
  CHECK((io.read(0x0B) & 0x02) == 0x02);  // IF bit 1 now latched
  CHECK((io.read(0x0F) & 0x20) == 0x20);  // PB5 mirrors TP's new (high) level

  io.write(0x0B, 0x00);  // ROM clears IF explicitly before re-polling
  io.advanceManualRtcClock(halfPeriod * 1.01);  // one more half-period -> falling edge only
  CHECK((io.read(0x0B) & 0x02) == 0x00);  // falling edge must not re-set IF bit 1
  CHECK((io.read(0x0F) & 0x20) == 0x00);  // PB5 now low again
}

void testRtcTimeSetThenTimeReadRoundTrip() {
  // Exercises the full documented protocol end to end: Time Set shifts a
  // new value in and (on leaving Time Set) commits it as the chip's live
  // time; Time Read then snapshots that live time back into the shift
  // register for Register Shift to read back out. Day 15 is used
  // specifically so it's valid in every month (avoids a Feb-29 edge case),
  // since month/year aren't independently controlled here -- the chip
  // itself has no year field at all (see commitShiftRegisterToTime()).
  pc1500::IoPortController io;
  uint64_t setValue = rtcPackBcd40(/*month=*/5, /*dow=*/2, /*day=*/15, /*hour=*/10,
                                    /*minute=*/30, /*second=*/45);

  rtcLatchCommand(io, /*c0=*/false, /*c1=*/true, /*c2=*/false);  // Time Set & Counter Hold
  rtcShiftIn(io, setValue);
  rtcLatchCommand(io, /*c0=*/false, /*c1=*/false, /*c2=*/false);  // Register Hold -> commits

  rtcLatchCommand(io, /*c0=*/true, /*c1=*/true, /*c2=*/false);   // Time Read -> snapshot
  rtcLatchCommand(io, /*c0=*/true, /*c1=*/false, /*c2=*/false);  // Register Shift -> can clock out
  uint64_t readBack = rtcShiftOut(io);

  auto nibble = [readBack](int shift) { return static_cast<int>((readBack >> shift) & 0x0F); };
  CHECK(nibble(36) == 5);                        // month
  CHECK(nibble(28) * 10 + nibble(24) == 15);      // day
  CHECK(nibble(20) * 10 + nibble(16) == 10);      // hour
  CHECK(nibble(12) * 10 + nibble(8) == 30);       // minute
  CHECK(nibble(4) * 10 + nibble(0) == 45);        // second
}

void testMe1MirrorsIoPortControllerWhereAd12Ad13BothSet() {
  // CS0/CS1/CS2 tied to AD12/AD13/(fixed) -- confirmed on real hardware
  // that AD14/AD15 aren't decoded (F00AH/F00BH and B00AH/B00BH read back
  // identical, live values: F000H and B000H agree on bits 12-13, differing
  // only in bit 14). Addresses where bits 12-13 aren't both set stay
  // genuinely unmapped.
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.writeME1(0xF00C, 0xFF);  // DDA = all outputs
  bus.writeME1(0xF00E, 0x55);  // OPA = 0x55
  CHECK(bus.readME1(0xF00E) == 0x55);
  CHECK(bus.readME1(0xB00E) == 0x55);  // same register, aliased address (bits 12-13 agree)
  bus.writeME1(0xB00D, 0xAA);          // write DDB via a different aliased address
  CHECK(bus.readME1(0xF00D) == 0xAA);  // visible through the "canonical" address too
  CHECK(bus.readME1(0x1234) == 0xFF);  // bits 12-13 not both set -- genuinely unmapped
  bus.writeME1(0x1234, 0x00);
  CHECK(bus.readME1(0x1234) == 0xFF);  // write outside the decode is a no-op
}

void testExtensionRam4800WindowSizesGateMappedRange() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  // Off by default -- matches the no-module behavior every other test
  // above already assumes. Default machine variant is PC1500, so the
  // expansion window's base is 4800H and its full span is 10K -- see
  // testExtensionWindowFollowsMachineVariant below for the PC-1500A case.
  CHECK(bus.extRamExtSize() == 0);
  CHECK(bus.readME0(0x4800) == 0xFF);
  bus.writeME0(0x4800, 0x42);
  CHECK(bus.readME0(0x4800) == 0xFF);  // write ignored while disabled

  // A partial (4K) module only maps the start of the window -- the rest
  // stays unmapped, matching a physically smaller module not filling its
  // whole socket.
  bus.setExtRamExtSize(0x1000);
  bus.writeME0(0x4800, 0x42);
  CHECK(bus.readME0(0x4800) == 0x42);
  bus.writeME0(0x57FF, 0x11);  // last byte of the 4K module (4800H + 1000H - 1)
  CHECK(bus.readME0(0x57FF) == 0x11);
  CHECK(bus.readME0(0x5800) == 0xFF);  // one past the 4K module -- still unmapped
  bus.writeME0(0x5800, 0x99);
  CHECK(bus.readME0(0x5800) == 0xFF);

  // Growing to the full 10K window maps the rest too, without disturbing
  // what was already there.
  bus.setExtRamExtSize(bus.extRamExtWindowMaxSize());
  CHECK(bus.readME0(0x4800) == 0x42);  // preserved from the 4K-module write above
  bus.writeME0(0x6FFF, 0x99);          // last byte of the full 10K window
  CHECK(bus.readME0(0x6FFF) == 0x99);
  CHECK(bus.readME0(0x7000) == 0xFF);  // one past the window -- untouched, still its own default

  // Shrinking back to disabled doesn't clear the underlying bytes.
  bus.setExtRamExtSize(0);
  CHECK(bus.readME0(0x4800) == 0xFF);  // reads as unmapped again
  bus.setExtRamExtSize(bus.extRamExtWindowMaxSize());
  CHECK(bus.readME0(0x4800) == 0x42);  // but the byte was preserved underneath
}

void testExtensionRam0000WindowSizeGatesMappedRange() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  CHECK(bus.extRam0000Size() == 0);
  CHECK(bus.readME0(0x0000) == 0xFF);
  bus.writeME0(0x0000, 0x77);
  CHECK(bus.readME0(0x0000) == 0xFF);  // write ignored while disabled

  bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);
  bus.writeME0(0x0000, 0x77);
  CHECK(bus.readME0(0x0000) == 0x77);
  bus.writeME0(0x3FFF, 0x88);  // last byte of the 16K window
  CHECK(bus.readME0(0x3FFF) == 0x88);
}

// CE-163: 32K module, banked two 16K halves at a time into 0000H-3FFFH.
// Bank select is a pure write-triggered address-line latch at 5800H-5FFFH
// (even address -> bank 0, odd -> bank 1; the byte value is irrelevant),
// and each bank has to keep its own contents independently -- the whole
// point of having two banks, as opposed to just a size gate like the other
// two extension-RAM windows.
void testCe163BankSwitchingKeepsBanksIndependent() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  CHECK(bus.ce163Enabled() == false);
  CHECK(bus.ce163Bank() == 0);
  CHECK(bus.readME0(0x0000) == 0xFF);  // disabled -- unmapped, matching stock hardware
  bus.writeME0(0x0000, 0x11);
  CHECK(bus.readME0(0x0000) == 0xFF);  // write ignored while disabled

  bus.setCe163Enabled(true);
  CHECK(bus.readME0(0x0000) == 0xFF);  // real RAM's confirmed power-up default, not 0x00
  CHECK(bus.readME0(0x3FFF) == 0xFF);  // last byte of the window

  // Bank 0 is active by default -- write a recognizable pattern.
  bus.writeME0(0x0000, 0xAA);
  bus.writeME0(0x3FFF, 0xBB);
  CHECK(bus.readME0(0x0000) == 0xAA);
  CHECK(bus.readME0(0x3FFF) == 0xBB);

  // A write to an ODD address in 5800H-5FFFH selects bank 1 -- the value
  // written is irrelevant (0x00 here), and nothing is actually stored at
  // that address (it stays unmapped -- see the mutual-exclusion check
  // below, and 4800H-6FFFH generally when CE-163 is enabled).
  bus.writeME0(0x5801, 0x00);
  CHECK(bus.ce163Bank() == 1);
  CHECK(bus.readME0(0x0000) == 0xFF);  // bank 1's own fresh contents, not bank 0's 0xAA
  CHECK(bus.readME0(0x3FFF) == 0xFF);

  // Write a DIFFERENT pattern into bank 1.
  bus.writeME0(0x0000, 0xCC);
  bus.writeME0(0x3FFF, 0xDD);
  CHECK(bus.readME0(0x0000) == 0xCC);
  CHECK(bus.readME0(0x3FFF) == 0xDD);

  // A write to an EVEN address in 5800H-5FFFH switches back to bank 0 --
  // its earlier contents must still be there, untouched by bank 1's writes.
  bus.writeME0(0x5800, 0xFF);  // value still irrelevant
  CHECK(bus.ce163Bank() == 0);
  CHECK(bus.readME0(0x0000) == 0xAA);
  CHECK(bus.readME0(0x3FFF) == 0xBB);

  // Switch to bank 1 one more time to confirm ITS contents also survived
  // the round trip (not just bank 0's).
  bus.writeME0(0x5801, 0x00);
  CHECK(bus.readME0(0x0000) == 0xCC);
  CHECK(bus.readME0(0x3FFF) == 0xDD);
}

// Real hardware has one expansion port -- CE-163 can't coexist with either
// existing extension-RAM window. Enabling one clears the other; enforced
// from both directions so Bus state can't end up contradictory regardless
// of call order (see each setter's own comment in bus.h).
void testCe163IsMutuallyExclusiveWithOtherExtensionRam() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);

  bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);
  CHECK(bus.extRam0000Size() == pc1500::Bus::kExtRam0000WindowSize);
  bus.setCe163Enabled(true);
  CHECK(bus.ce163Enabled() == true);
  CHECK(bus.extRam0000Size() == 0);  // cleared by enabling CE-163

  bus.setExtRamExtSize(bus.extRamExtWindowMaxSize());
  CHECK(bus.extRamExtSize() == bus.extRamExtWindowMaxSize());
  CHECK(bus.ce163Enabled() == false);  // cleared by setting a nonzero expansion-window size

  bus.setCe163Enabled(true);
  CHECK(bus.ce163Enabled() == true);
  CHECK(bus.extRamExtSize() == 0);  // cleared by enabling CE-163 again

  bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);
  CHECK(bus.ce163Enabled() == false);  // cleared by setting a nonzero 0000H size
}

// Regression test for a real bug: selecting "None" (bytes == 0) in the
// Extension RAM (0000H) submenu is itself one of the five mutually-
// exclusive alternatives in that same submenu, so it must clear
// CE-163/CE-155/CE-168N too -- not just nonzero sizes. Before this fix,
// setExtRam0000Size only cleared the others on bytes != 0, so
// setExtRam0000Size(0) while CE-163 was enabled left CE-163 (and its
// gating of the 0000H window via isUnmapped()'s !ce163Enabled_ check)
// silently in effect -- confirmed live: the "None" menu item appeared to
// do nothing while CE-163 was active.
void testExtRam0000SizeZeroClearsCe163AndCe155() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);

  bus.setCe163Enabled(true);
  CHECK(bus.ce163Enabled() == true);
  bus.setExtRam0000Size(0);
  CHECK(bus.ce163Enabled() == false);  // "None" must turn CE-163 back off
  CHECK(bus.extRam0000Size() == 0);

  bus.setCe155Enabled(true);
  CHECK(bus.ce155Enabled() == true);
  bus.setExtRam0000Size(0);
  CHECK(bus.ce155Enabled() == false);  // same for CE-155
  CHECK(bus.extRam0000Size() == 0);

  bus.setCe168nEnabled(4, 2);
  CHECK(bus.ce168nEnabled() == true);
  bus.setExtRam0000Size(0);
  CHECK(bus.ce168nEnabled() == false);  // same for CE-168N
  CHECK(bus.extRam0000Size() == 0);
}

// Same as above, but for CE-155 -- and confirming CE-163/CE-155 clear
// *each other* too (not just the two plain-size windows), completing that
// part of the exclusion web (see testCe168nIsMutuallyExclusiveWithOther
// ExtensionRamAndCe163 below for CE-168N's own share of it).
void testCe155IsMutuallyExclusiveWithOtherExtensionRam() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);

  bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);
  bus.setCe155Enabled(true);
  CHECK(bus.ce155Enabled() == true);
  CHECK(bus.extRam0000Size() == 0);  // cleared by enabling CE-155

  bus.setExtRamExtSize(bus.extRamExtWindowMaxSize());
  CHECK(bus.ce155Enabled() == false);  // cleared by setting a nonzero expansion-window size

  bus.setCe155Enabled(true);
  CHECK(bus.extRamExtSize() == 0);  // cleared by enabling CE-155 again

  bus.setCe163Enabled(true);
  CHECK(bus.ce163Enabled() == true);
  CHECK(bus.ce155Enabled() == false);  // cleared by enabling CE-163

  bus.setCe155Enabled(true);
  CHECK(bus.ce155Enabled() == true);
  CHECK(bus.ce163Enabled() == false);  // cleared by enabling CE-155 again
}

// CE-168N: same window/bank-select mechanism as CE-163, generalized to a
// parametrized bank count and a first-read-only-bank boundary. Banks below
// the boundary behave exactly like CE-163's banks (independent contents,
// freely writable); banks at/above it simulate flash -- writeME0 discards
// writes silently, but loadME0 (the `loadbinary` FIFO command's path) can
// still seed their initial contents.
void testCe168nBankSwitchingRespectsReadOnlyBanks() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  CHECK(bus.ce168nEnabled() == false);
  CHECK(bus.ce168nBank() == 0);
  CHECK(bus.readME0(0x0000) == 0xFF);  // disabled -- unmapped, matching stock hardware

  bus.setCe168nEnabled(4, 2);  // 4 banks, banks 2-3 read-only
  CHECK(bus.ce168nEnabled() == true);
  CHECK(bus.readME0(0x0000) == 0xFF);  // real RAM's confirmed power-up default

  // Bank 0 (writable) is active by default.
  bus.writeME0(0x0000, 0xAA);
  CHECK(bus.readME0(0x0000) == 0xAA);

  // Select bank 1 (still writable) -- same address-line trigger as CE-163,
  // generalized via addr % banks rather than addr & mask.
  bus.writeME0(0x5801, 0x00);
  CHECK(bus.ce168nBank() == 1);
  CHECK(bus.readME0(0x0000) == 0xFF);  // bank 1's own fresh contents
  bus.writeME0(0x0000, 0xCC);
  CHECK(bus.readME0(0x0000) == 0xCC);

  // Bank 0's contents survived bank 1's writes.
  bus.writeME0(0x5800, 0x00);
  CHECK(bus.ce168nBank() == 0);
  CHECK(bus.readME0(0x0000) == 0xAA);

  // Select bank 2 -- read-only. A normal CPU write (writeME0) is silently
  // discarded, not an error.
  bus.writeME0(0x5802, 0x00);
  CHECK(bus.ce168nBank() == 2);
  CHECK(bus.readME0(0x0000) == 0xFF);
  bus.writeME0(0x0000, 0xEE);
  CHECK(bus.readME0(0x0000) == 0xFF);  // write ignored -- bank is read-only

  // loadME0 (the scripting/preset load path) can still seed bank 2's
  // content, bypassing the read-only gate.
  uint8_t flashByte = 0x42;
  bus.loadME0(0x0000, &flashByte, 1);
  CHECK(bus.readME0(0x0000) == 0x42);
  // ...and a subsequent CPU write still can't overwrite it.
  bus.writeME0(0x0000, 0x00);
  CHECK(bus.readME0(0x0000) == 0x42);

  // Bank 3 is independently read-only too, with its own storage.
  bus.writeME0(0x5803, 0x00);
  CHECK(bus.ce168nBank() == 3);
  CHECK(bus.readME0(0x0000) == 0xFF);
  bus.writeME0(0x0000, 0x99);
  CHECK(bus.readME0(0x0000) == 0xFF);
}

// Same one-expansion-port reasoning as CE-163/CE-155: mutually exclusive
// with both extension-RAM windows, CE-163, and CE-155, enforced from every
// direction.
void testCe168nIsMutuallyExclusiveWithOtherExtensionRamAndCe163() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);

  bus.setExtRam0000Size(pc1500::Bus::kExtRam0000WindowSize);
  bus.setCe168nEnabled(4, 2);
  CHECK(bus.ce168nEnabled() == true);
  CHECK(bus.extRam0000Size() == 0);  // cleared by enabling CE-168N

  bus.setExtRamExtSize(bus.extRamExtWindowMaxSize());
  CHECK(bus.ce168nEnabled() == false);  // cleared by setting a nonzero expansion-window size

  bus.setCe168nEnabled(4, 2);
  CHECK(bus.extRamExtSize() == 0);  // cleared by enabling CE-168N again

  bus.setCe163Enabled(true);
  CHECK(bus.ce163Enabled() == true);
  CHECK(bus.ce168nEnabled() == false);  // cleared by enabling CE-163

  bus.setCe168nEnabled(4, 2);
  CHECK(bus.ce168nEnabled() == true);
  CHECK(bus.ce163Enabled() == false);  // cleared by enabling CE-168N again

  bus.setCe155Enabled(true);
  CHECK(bus.ce155Enabled() == true);
  CHECK(bus.ce168nEnabled() == false);  // cleared by enabling CE-155

  bus.setCe168nEnabled(4, 2);
  CHECK(bus.ce168nEnabled() == true);
  CHECK(bus.ce155Enabled() == false);  // cleared by enabling CE-168N again
}

// PC-1500A: built-in RAM grows from 2K to 6K (absorbing what would
// otherwise be the expansion window's own first 4K), and the expansion
// window itself shrinks from 10K to 6K, based at 5800H instead of 4800H
// -- see Bus::extRamExtBase()'s own comment for the full derivation.
void testExtensionWindowFollowsMachineVariant() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500A);
  CHECK(bus.extRamExtBase() == 0x5800);
  CHECK(bus.extRamExtWindowMaxSize() == 0x1800);  // 6K

  // 4800H-57FFH is now built-in RAM (the PC-1500A's extra 4K), always
  // mapped regardless of extRamExtSize_ -- unlike on a stock PC-1500,
  // where this same range is gated by the expansion window's own size
  // (see testExtensionRam4800WindowSizesGateMappedRange above).
  CHECK(bus.readME0(0x4800) == 0xFF);  // mapped, just at its 0xFF power-up default
  bus.writeME0(0x4800, 0x42);
  CHECK(bus.readME0(0x4800) == 0x42);
  bus.writeME0(0x57FF, 0x11);  // last byte of the built-in 6K
  CHECK(bus.readME0(0x57FF) == 0x11);

  // 5800H is the expansion window's own base now -- gated by
  // extRamExtSize_, same as 4800H would be on a stock PC-1500.
  CHECK(bus.readME0(0x5800) == 0xFF);  // unmapped (0 bytes configured)
  bus.writeME0(0x5800, 0x99);
  CHECK(bus.readME0(0x5800) == 0xFF);  // write ignored while disabled

  bus.setExtRamExtSize(bus.extRamExtWindowMaxSize());  // 6K, the full window
  bus.writeME0(0x5800, 0x99);
  CHECK(bus.readME0(0x5800) == 0x99);
  bus.writeME0(0x6FFF, 0x77);  // last byte of the 6K window
  CHECK(bus.readME0(0x6FFF) == 0x77);
  CHECK(bus.readME0(0x7000) == 0xFF);  // one past the window -- untouched (fixed peripheral region)
}

// A real regression risk: the CE-163 trigger range must move with the
// machine variant, and the PC-1500's own range must stop firing once a
// PC-1500A is selected (not just "also respond to the new range").
void testCe163TriggerRangeFollowsMachineVariant() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500A);
  bus.setCe163Enabled(true);
  CHECK(bus.ce163Bank() == 0);

  // The PC-1500's own trigger range (5800H-5FFFH) must NOT fire once the
  // variant is PC-1500A.
  bus.writeME0(0x5801, 0x00);  // odd address -- would select bank 1 on a PC-1500
  CHECK(bus.ce163Bank() == 0);  // unchanged

  // The PC-1500A's own trigger range (6800H-6FFFH) does fire.
  bus.writeME0(0x6801, 0x00);  // odd address -- selects bank 1
  CHECK(bus.ce163Bank() == 1);
  bus.writeME0(0x6800, 0x00);  // even address -- selects bank 0
  CHECK(bus.ce163Bank() == 0);
}

// CE-155's real hardware topology isolates just the top 2K of the
// 0000H-3FFFH window (3800H-3FFFH), unlike the generic 0000H-window
// (always left-aligned from 0000H) -- confirmed for both machine
// variants, since only the *expansion*-window half of CE-155 moves.
void testCe155IsolatesOnlyTopOfLowerWindow() {
  for (pc1500::Bus::MachineVariant variant :
       {pc1500::Bus::MachineVariant::PC1500, pc1500::Bus::MachineVariant::PC1500A}) {
    pc1500::Keyboard kb;
    pc1500::Bus bus(kb);
    bus.setMachineVariant(variant);
    bus.setCe155Enabled(true);

    CHECK(bus.readME0(0x0000) == 0xFF);
    bus.writeME0(0x0000, 0x42);
    CHECK(bus.readME0(0x0000) == 0xFF);  // write ignored -- not part of CE-155's real topology
    CHECK(bus.readME0(0x37FF) == 0xFF);
    bus.writeME0(0x37FF, 0x42);
    CHECK(bus.readME0(0x37FF) == 0xFF);

    bus.writeME0(0x3800, 0x11);
    CHECK(bus.readME0(0x3800) == 0x11);
    bus.writeME0(0x3FFF, 0x22);
    CHECK(bus.readME0(0x3FFF) == 0x22);

    // The expansion window is filled to exactly 6K, at whichever base
    // this variant uses.
    uint16_t base = bus.extRamExtBase();
    bus.writeME0(base, 0x33);
    CHECK(bus.readME0(base) == 0x33);
    uint16_t lastByte = static_cast<uint16_t>(base + 0x1800 - 1);  // last byte of the 6K
    bus.writeME0(lastByte, 0x44);
    CHECK(bus.readME0(lastByte) == 0x44);
    // One past CE-155's own 6K -- on a stock PC-1500 (10K window), this
    // is still inside the window but beyond what CE-155 itself uses, so
    // it stays unmapped; on a PC-1500A the window IS exactly 6K, so
    // there's nothing further to check within it.
    uint16_t onePast = static_cast<uint16_t>(base + 0x1800);
    if (onePast <= 0x6FFF) {
      CHECK(bus.readME0(onePast) == 0xFF);
    }
  }
}

// Switching variants can't silently leave an out-of-range expansion-
// window size selected (e.g. 10K chosen on a PC-1500, then switching to
// a PC-1500A's 6K-max window).
void testMachineVariantClampsExtRamExtSize() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  bus.setExtRamExtSize(bus.extRamExtWindowMaxSize());  // 10K, full PC-1500 window
  CHECK(bus.extRamExtSize() == 0x2800);

  bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500A);
  CHECK(bus.extRamExtSize() == 0x1800);  // clamped down to the new 6K max

  // A size already within the new max is left untouched.
  bus.setExtRamExtSize(0x1000);
  bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500);
  CHECK(bus.extRamExtSize() == 0x1000);
}

// Unlike a plain loadrommodule-style module (testRomIsReadOnly-equivalent
// for the 0x8000-0xBFFF region -- writes there are always discarded), a
// module with a data window genuinely has RAM: writes stick, reads see
// them back, and PV gating still applies exactly like the ROM sub-range.
void testExpansionModuleDataWindowIsReadWrite() {
  pc1500::Keyboard kb;
  pc1500::Bus bus(kb);
  uint8_t romBytes[] = {0x55, 0xAA};
  bus.loadExpansionModule(0, romBytes, sizeof(romBytes), /*base=*/0x9000, /*requirePv=*/false,
                           /*usePuBank=*/false, /*dataWindowBase=*/0x8000,
                           /*dataWindowSize=*/0x1000, /*instructionAddr=*/0x8FFF);

  CHECK(bus.readME0(0x9000) == 0x55);
  bus.writeME0(0x9000, 0x99);
  CHECK(bus.readME0(0x9000) == 0x55);  // ROM sub-range still read-only

  CHECK(bus.readME0(0x8000) == 0xFF);  // real RAM's confirmed power-up default
  bus.writeME0(0x8000, 0x42);
  CHECK(bus.readME0(0x8000) == 0x42);
  bus.writeME0(0x81FF, 0x77);
  CHECK(bus.readME0(0x81FF) == 0x77);

  bus.setPv(true);
  CHECK(bus.readME0(0x8000) == 0xFF);  // module not selected at this PV level
  bus.writeME0(0x8000, 0x11);
  CHECK(bus.readME0(0x8000) == 0xFF);  // write also ignored -- wrong PV level
  bus.setPv(false);
  CHECK(bus.readME0(0x8000) == 0x42);  // unaffected by the ignored write above
}

// ---------------------------------------------------------------------
// ExpansionMock SD-card commands, backed by a real host directory (not an
// embedded mock list -- see ExpansionMock::setRootDir's own comment).

fs::path makeTempTestDir(const char* name) {
  fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);  // clean slate if a previous run left it behind
  fs::create_directories(dir);
  return dir;
}

pc1500::Bus* makeExpansionBus(pc1500::Keyboard& kb, const fs::path& rootDir) {
  static uint8_t romBytes[] = {0x55};
  auto* bus = new pc1500::Bus(kb);
  bus->loadExpansionModule(0, romBytes, sizeof(romBytes), 0x9000, false, false, 0x8000, 0x1000,
                            0x8FFF);
  if (!rootDir.empty()) bus->expansionMock().setRootDir(rootDir);
  return bus;
}

// Writes a 2-byte BE length prefix at window offset 0, then `text`'s raw
// bytes right after -- the wire format every filename/volume-name
// argument uses (StringFromBuffer's own convention in main.c).
void writeLengthPrefixedArg(pc1500::Bus& bus, const std::string& text) {
  bus.writeME0(0x8000, static_cast<uint8_t>(text.size() >> 8));
  bus.writeME0(0x8001, static_cast<uint8_t>(text.size() & 0xFF));
  for (size_t i = 0; i < text.size(); i++) {
    bus.writeME0(static_cast<uint16_t>(0x8002 + i), static_cast<uint8_t>(text[i]));
  }
}

uint8_t triggerCommand(pc1500::Bus& bus, uint8_t cmd) {
  bus.writeME0(0x8FFF, cmd);
  return bus.readME0(0x8FFF);
}

// A write to instructionAddr triggers ExpansionMock::processCommand
// synchronously -- verifies EXP_COMMAND_LIST_SD_DIR's real wire format
// (2-byte BE count, then per entry: 16-byte space-padded name, 10-byte
// pre-rendered decimal size text, 4-byte BE binary size) lands correctly
// against a real file in a real directory, matching PC_EXP.h/main.c's own
// layout.
void testExpansionModuleListSdDirReflectsRealDirectory() {
  fs::path dir = makeTempTestDir("pc1500emu_bus_test_listdir");
  {
    std::ofstream f(dir / "HELLO.TXT", std::ios::binary);
    f << "Hi there!";  // 9 bytes
  }
  pc1500::Keyboard kb;
  pc1500::Bus* bus = makeExpansionBus(kb, dir);

  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandListSdDir) ==
        pc1500::ExpansionMock::kStatusSuccess);

  CHECK(bus->readME0(0x8000) == 0x00);  // count, BE
  CHECK(bus->readME0(0x8001) == 0x01);

  const char* name0 = "HELLO.TXT";
  for (int i = 0; i < 16; i++) {
    uint8_t expected = (i < 9) ? static_cast<uint8_t>(name0[i]) : ' ';
    CHECK(bus->readME0(static_cast<uint16_t>(0x8002 + i)) == expected);
  }
  const char* sizeText0 = "         9";
  for (int i = 0; i < 10; i++) {
    CHECK(bus->readME0(static_cast<uint16_t>(0x8012 + i)) == static_cast<uint8_t>(sizeText0[i]));
  }
  CHECK(bus->readME0(0x801C) == 0x00);
  CHECK(bus->readME0(0x801D) == 0x00);
  CHECK(bus->readME0(0x801E) == 0x00);
  CHECK(bus->readME0(0x801F) == 0x09);

  // Summary line right after the one entry: offset 2 + 1*30 = 32 = 0x8020.
  // Free space is a real (unpredictable) host-disk number, so only the
  // prefix up to it is checked.
  const char* summaryPrefix = "1 FILES 9B ";
  for (size_t i = 0; i < std::string(summaryPrefix).size(); i++) {
    CHECK(bus->readME0(static_cast<uint16_t>(0x8020 + i)) ==
          static_cast<uint8_t>(summaryPrefix[i]));
  }

  delete bus;
  fs::remove_all(dir);
}

// With no rootDir configured, every SD command reports EXP_STATUS_ERROR
// ("no card inserted") rather than silently doing nothing or crashing.
void testExpansionModuleWithNoRootDirReportsError() {
  pc1500::Keyboard kb;
  pc1500::Bus* bus = makeExpansionBus(kb, "");
  writeLengthPrefixedArg(*bus, "TEST.BIN");
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandCreateSdFile) ==
        pc1500::ExpansionMock::kStatusError);
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandGetSdFreeSpace) ==
        pc1500::ExpansionMock::kStatusError);
  delete bus;
}

// Full create -> write -> close -> open-read -> read -> close round trip
// against real files, matching main.c's own single-open-file protocol.
void testExpansionModuleFileWriteReadRoundTrip() {
  fs::path dir = makeTempTestDir("pc1500emu_bus_test_fileio");
  pc1500::Keyboard kb;
  pc1500::Bus* bus = makeExpansionBus(kb, dir);

  writeLengthPrefixedArg(*bus, "TEST.BIN");
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandCreateSdFile) ==
        pc1500::ExpansionMock::kStatusSuccess);
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandGetSdFileStatus) ==
        pc1500::ExpansionMock::kStatusSuccess);
  CHECK(bus->readME0(0x8000) == pc1500::ExpansionMock::kFileStatusOpenWrite);

  std::string payload = "Hello, PC-1500!";
  bus->writeME0(0x8000, static_cast<uint8_t>(payload.size() >> 8));
  bus->writeME0(0x8001, static_cast<uint8_t>(payload.size() & 0xFF));
  for (size_t i = 0; i < payload.size(); i++) {
    bus->writeME0(static_cast<uint16_t>(0x8002 + i), static_cast<uint8_t>(payload[i]));
  }
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandWriteToSdFile) ==
        pc1500::ExpansionMock::kStatusSuccess);

  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandCloseSdFile) ==
        pc1500::ExpansionMock::kStatusSuccess);
  uint32_t closedSize = (static_cast<uint32_t>(bus->readME0(0x8000)) << 24) |
                         (static_cast<uint32_t>(bus->readME0(0x8001)) << 16) |
                         (static_cast<uint32_t>(bus->readME0(0x8002)) << 8) |
                         bus->readME0(0x8003);
  CHECK(closedSize == payload.size());
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandGetSdFileStatus) ==
        pc1500::ExpansionMock::kStatusSuccess);
  CHECK(bus->readME0(0x8000) == pc1500::ExpansionMock::kFileStatusClosed);

  // The file genuinely exists on the host now, independent of the mock.
  CHECK(fs::exists(dir / "TEST.BIN"));
  CHECK(fs::file_size(dir / "TEST.BIN") == payload.size());

  writeLengthPrefixedArg(*bus, "TEST.BIN");
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandOpenSdFileRead) ==
        pc1500::ExpansionMock::kStatusSuccess);
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandGetSdFileSize) ==
        pc1500::ExpansionMock::kStatusSuccess);
  uint32_t openSize = (static_cast<uint32_t>(bus->readME0(0x8000)) << 24) |
                       (static_cast<uint32_t>(bus->readME0(0x8001)) << 16) |
                       (static_cast<uint32_t>(bus->readME0(0x8002)) << 8) |
                       bus->readME0(0x8003);
  CHECK(openSize == payload.size());

  bus->writeME0(0x8000, 0x00);
  bus->writeME0(0x8001, static_cast<uint8_t>(payload.size()));  // requestLen, BE, < 254
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandReadFromSdFile) ==
        pc1500::ExpansionMock::kStatusSuccess);
  uint16_t bytesRead = (static_cast<uint16_t>(bus->readME0(0x8000)) << 8) | bus->readME0(0x8001);
  CHECK(bytesRead == payload.size());
  for (size_t i = 0; i < payload.size(); i++) {
    CHECK(bus->readME0(static_cast<uint16_t>(0x8002 + i)) == static_cast<uint8_t>(payload[i]));
  }

  writeLengthPrefixedArg(*bus, "TEST.BIN");  // GET_SD_FILE_NAME doesn't take an arg, but
                                              // harmless to leave one queued -- confirms it's
                                              // ignored by reading from EXP_SCRATCH_PAGE instead
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandGetSdFileName) ==
        pc1500::ExpansionMock::kStatusSuccess);
  CHECK(bus->readME0(0x8100) == 8);  // "TEST.BIN" length
  const char* name = "TEST.BIN";
  for (int i = 0; i < 8; i++) {
    CHECK(bus->readME0(static_cast<uint16_t>(0x8101 + i)) == static_cast<uint8_t>(name[i]));
  }

  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandCloseSdFile) ==
        pc1500::ExpansionMock::kStatusSuccess);

  writeLengthPrefixedArg(*bus, "TEST.BIN");
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandRemoveSdFile) ==
        pc1500::ExpansionMock::kStatusSuccess);
  CHECK(!fs::exists(dir / "TEST.BIN"));

  delete bus;
  fs::remove_all(dir);
}

// FORMAT_SD_CARD deletes every regular file directly in rootDir -- verify
// against real files, and that it's genuinely destructive (by design).
void testExpansionModuleFormatDeletesAllFiles() {
  fs::path dir = makeTempTestDir("pc1500emu_bus_test_format");
  { std::ofstream(dir / "A.TXT", std::ios::binary) << "a"; }
  { std::ofstream(dir / "B.TXT", std::ios::binary) << "b"; }
  pc1500::Keyboard kb;
  pc1500::Bus* bus = makeExpansionBus(kb, dir);

  writeLengthPrefixedArg(*bus, "PC1500");  // volume name arg -- content unused by the mock
  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandFormatSdCard) ==
        pc1500::ExpansionMock::kStatusSuccess);
  CHECK(fs::is_empty(dir));

  delete bus;
  fs::remove_all(dir);
}

// Filenames straight off the wire are untrusted -- confirms path
// traversal / absolute-path / embedded-separator attempts are rejected
// (EXP_STATUS_ERROR) rather than escaping the configured root directory.
void testExpansionModuleRejectsUnsafeFilenames() {
  fs::path dir = makeTempTestDir("pc1500emu_bus_test_pathsafety");
  pc1500::Keyboard kb;
  pc1500::Bus* bus = makeExpansionBus(kb, dir);

  const char* unsafeNames[] = {"../ESCAPED.TXT", "..\\ESCAPED.TXT", "/etc/passwd",
                                "C:\\Windows\\evil.txt", "sub/dir.txt"};
  for (const char* name : unsafeNames) {
    writeLengthPrefixedArg(*bus, name);
    CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandCreateSdFile) ==
          pc1500::ExpansionMock::kStatusError);
  }
  // Nothing escaped into the parent of the sandbox directory.
  CHECK(!fs::exists(dir.parent_path() / "ESCAPED.TXT"));
  CHECK(fs::is_empty(dir));

  delete bus;
  fs::remove_all(dir);
}

// GET_SD_FREE_SPACE/GET_SD_VOLUME_SIZE return real (unpredictable) host-
// disk numbers -- just confirm they succeed and report something nonzero,
// not exact values.
void testExpansionModuleFreeSpaceAndVolumeSizeSucceed() {
  fs::path dir = makeTempTestDir("pc1500emu_bus_test_space");
  pc1500::Keyboard kb;
  pc1500::Bus* bus = makeExpansionBus(kb, dir);

  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandGetSdFreeSpace) ==
        pc1500::ExpansionMock::kStatusSuccess);
  uint32_t freeSpace = (static_cast<uint32_t>(bus->readME0(0x8000)) << 24) |
                        (static_cast<uint32_t>(bus->readME0(0x8001)) << 16) |
                        (static_cast<uint32_t>(bus->readME0(0x8002)) << 8) |
                        bus->readME0(0x8003);
  CHECK(freeSpace > 0);

  CHECK(triggerCommand(*bus, pc1500::ExpansionMock::kCommandGetSdVolumeSize) ==
        pc1500::ExpansionMock::kStatusSuccess);
  uint32_t volumeSize = (static_cast<uint32_t>(bus->readME0(0x8000)) << 24) |
                         (static_cast<uint32_t>(bus->readME0(0x8001)) << 16) |
                         (static_cast<uint32_t>(bus->readME0(0x8002)) << 8) |
                         bus->readME0(0x8003);
  CHECK(volumeSize > 0);

  delete bus;
  fs::remove_all(dir);
}

}  // namespace

int main() {
  testUnmappedRegionsReadHighAndIgnoreWrites();
  testMirroredRegionsAliasRealRam();
  testPC1500A_7C00RangeIsRealRam();
  testReserveAreaIsZeroedAtDefaultBase();
  testReserveAreaFollowsExtRam0000Config();
  testRomIsReadOnly();
  testRamRegionsAreReadWrite();
  testIoPortControllerDdaGatesOpaReadback();
  testOpcBit6ControlsBuzzerOn();
  testSerialTransmitSetsTdFlagAfterProgrammedDelay();
  testSerialTransmitDelayScalesWithClockDivisor();
  testSerialTransmitRegisterIsWriteOnly();
  testSerialReceiveReadClearsRdFlag();
  testRtcTpRateSelectAndIfBitLatch();
  testRtcTimeSetThenTimeReadRoundTrip();
  testMe1MirrorsIoPortControllerWhereAd12Ad13BothSet();
  testExtensionRam4800WindowSizesGateMappedRange();
  testExtensionRam0000WindowSizeGatesMappedRange();
  testCe163BankSwitchingKeepsBanksIndependent();
  testCe163IsMutuallyExclusiveWithOtherExtensionRam();
  testExtRam0000SizeZeroClearsCe163AndCe155();
  testCe155IsMutuallyExclusiveWithOtherExtensionRam();
  testCe168nBankSwitchingRespectsReadOnlyBanks();
  testCe168nIsMutuallyExclusiveWithOtherExtensionRamAndCe163();
  testExtensionWindowFollowsMachineVariant();
  testCe163TriggerRangeFollowsMachineVariant();
  testCe155IsolatesOnlyTopOfLowerWindow();
  testMachineVariantClampsExtRamExtSize();
  testExpansionModuleDataWindowIsReadWrite();
  testExpansionModuleListSdDirReflectsRealDirectory();
  testExpansionModuleWithNoRootDirReportsError();
  testExpansionModuleFileWriteReadRoundTrip();
  testExpansionModuleFormatDeletesAllFiles();
  testExpansionModuleRejectsUnsafeFilenames();
  testExpansionModuleFreeSpaceAndVolumeSizeSucceed();

  if (g_failures == 0) {
    std::printf("All tests passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
