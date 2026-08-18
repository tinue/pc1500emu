// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#endif

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_ttf.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_config.h"
#include "basic_text.h"
#include "basic_tokens.h"
#include "bus.h"
#include "keyboard.h"
#include "lcd.h"
#include "lh5801.h"
#include "mac_activate.h"
#include "state_file.h"
#include "text_loader.h"
#include "tinyfiledialogs.h"

namespace {

// Keystroke-tap timing, the QueuedKeyAction type, charToTapActions, and the
// BASIC program load/save helpers (findBasicProgramEnd,
// readBasicProgramBytes, saveBasicProgram, saveBasicTextFile,
// loadBasicProgram, typeBasicProgramText, kBasicProgramStart,
// kProgramEndPointerAddr) all live in src/basic/text_loader.h now -- shared
// with tests/basic_load_roundtrip_test.cpp, which needs to drive the ROM's
// line editor the exact same way this file does, without a GUI. Pulled
// into unqualified scope here so every existing call site below (the
// interactive symbolActionQueue, calltrace, entertrace, and friends) keeps
// compiling unchanged.
using pc1500::basic::charToTapActions;
using pc1500::basic::kIdleFrames;
using pc1500::basic::kTapFrames;
using pc1500::basic::loadBasicProgram;
using pc1500::basic::QueuedKeyAction;
using pc1500::basic::readBasicProgramBytes;
using pc1500::basic::saveBasicProgram;
using pc1500::basic::saveBasicTextFile;
using pc1500::basic::typeBasicProgramText;

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(len > 0 ? len - 1 : 0, L'\0');
  if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
  return w;
}

std::string wideToUtf8(const wchar_t* w) {
  if (!w || !*w) return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(len > 0 ? len - 1 : 0, '\0');
  if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
  return s;
}

// Modern Explorer-style file picker (IFileOpenDialog/IFileSaveDialog, the
// "Common Item Dialog" available since Vista) -- NOT tinyfiledialogs'
// GetOpenFileName/GetSaveFileName (comdlg32), which is what it uses on
// Windows. tinyfiledialogs (vendored, third_party/tinyfiledialogs/) is
// v2.9.3 and has no IFileOpenDialog backend, so Windows bypasses it
// entirely rather than patching vendored code; non-Windows builds still go
// through tinyfiledialogs (requestPick's #else branch, in main()).
//
// A naive Show()-then-GetResult() implementation (an earlier version of
// this function) confirmed via timing instrumentation that Show() itself
// can block for several seconds on this machine -- unaffected by
// FOS_DONTADDTORECENT, and identical whether comdlg32 or IFileOpenDialog is
// used, so not just legacy-dialog-API overhead. Three disconnected mapped
// network drives are the suspected cause (Explorer's Common Item Dialog
// populates/refreshes "This PC" in its nav pane, including a status check
// per drive letter, as part of showing/closing), but that's not fully
// root-caused.
//
// So instead of waiting for Show() to return at all, this hooks
// IFileDialogEvents::OnFileOk -- which fires the instant the user's
// selection is *accepted* (double-click, or the Open/Save button),
// synchronously, from inside Show()'s own internal message loop, before
// whatever Show() does internally afterward (recent-items bookkeeping,
// AV/shell scanning, or whatever the real cause turns out to be) has a
// chance to run. `promise` is fulfilled right there, before Show() itself
// has returned -- the caller (requestPick, in main()) waits on the
// promise's future, not on this function's return, so it gets the result
// as soon as the user's pick is accepted rather than whenever Show()
// eventually unwinds. This function keeps running in the background on its
// own detached thread regardless (Show() has no way to be interrupted
// early), but nothing downstream waits on it anymore.
class FileDialogEventHandler : public IFileDialogEvents {
 public:
  explicit FileDialogEventHandler(std::shared_ptr<std::promise<std::optional<std::string>>> promise)
      : promise_(std::move(promise)) {}

  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == __uuidof(IFileDialogEvents)) {
      *ppv = static_cast<IFileDialogEvents*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
  IFACEMETHODIMP_(ULONG) Release() override {
    ULONG r = --refCount_;
    if (r == 0) delete this;
    return r;
  }

  IFACEMETHODIMP OnFileOk(IFileDialog* pfd) override {
    std::optional<std::string> path;
    IShellItem* item = nullptr;
    if (SUCCEEDED(pfd->GetResult(&item))) {
      PWSTR pathOut = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathOut))) {
        path = wideToUtf8(pathOut);
        CoTaskMemFree(pathOut);
      }
      item->Release();
    }
    fulfill(std::move(path));
    return S_OK;
  }
  IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
  IFACEMETHODIMP OnFolderChange(IFileDialog*) override { return S_OK; }
  IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
  IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override {
    return S_OK;
  }
  IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
  IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override {
    return S_OK;
  }

  // Also called after Show() returns (cancel, or anything else that closes
  // the dialog without OnFileOk ever firing) -- guarded so whichever of the
  // two calls happens first (OnFileOk, or this fallback) is the one that
  // actually resolves the promise; std::promise::set_value would otherwise
  // throw on a second call.
  void fulfill(std::optional<std::string> path) {
    bool expected = false;
    if (fulfilled_.compare_exchange_strong(expected, true)) {
      promise_->set_value(std::move(path));
    }
  }

 private:
  std::atomic<ULONG> refCount_{1};
  std::atomic<bool> fulfilled_{false};
  std::shared_ptr<std::promise<std::optional<std::string>>> promise_;
};

// Runs entirely on its own detached worker thread -- see requestPick, in
// main(), which is the only caller.
void nativeWinPickPathEarlyEvent(bool save, std::string title, std::vector<const char*> patterns,
                                  std::string description, std::string defaultPath,
                                  std::shared_ptr<std::promise<std::optional<std::string>>> promise) {
  // COINIT_APARTMENTTHREADED: IFileDialog::Show must run on an STA thread
  // (it pumps its own nested message loop while modal). This runs on a
  // fresh thread per pick (never reused), so there's no prior
  // CoInitializeEx on it to collide with in practice, but RPC_E_CHANGED_MODE
  // (some other code on this thread already initialized COM differently)
  // is checked anyway, for the same reason it'd matter if that ever changed.
  HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool weInitialized = (hrInit == S_OK || hrInit == S_FALSE);

  IFileDialog* pfd = nullptr;
  HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&pfd))
                     : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&pfd));
  FileDialogEventHandler* events = new FileDialogEventHandler(promise);
  if (SUCCEEDED(hr)) {
    // One filter spec joining every pattern (e.g. "*.BAS;*.bas") under the
    // caller's description, plus an "All Files" fallback -- matches what
    // tinyfiledialogs itself always appended, so behavior doesn't change
    // for callers.
    std::wstring patternsJoined;
    for (size_t i = 0; i < patterns.size(); i++) {
      if (i) patternsJoined += L";";
      patternsJoined += utf8ToWide(patterns[i]);
    }
    std::wstring descW = utf8ToWide(description);
    COMDLG_FILTERSPEC filterSpec[2] = {
        {descW.c_str(), patternsJoined.c_str()},
        {L"All Files", L"*.*"},
    };
    pfd->SetFileTypes(2, filterSpec);
    pfd->SetFileTypeIndex(1);
    std::wstring titleW = utf8ToWide(title);
    pfd->SetTitle(titleW.c_str());

    if (!defaultPath.empty()) {
      std::filesystem::path p(defaultPath);
      if (p.has_parent_path()) {
        IShellItem* folderItem = nullptr;
        // Best-effort: a folder that no longer exists just leaves the
        // dialog at its own last-used location instead of failing outright.
        if (SUCCEEDED(SHCreateItemFromParsingName(p.parent_path().wstring().c_str(), nullptr,
                                                    IID_PPV_ARGS(&folderItem)))) {
          pfd->SetFolder(folderItem);
          folderItem->Release();
        }
      }
      if (p.has_filename()) pfd->SetFileName(p.filename().wstring().c_str());
    }

    DWORD cookie = 0;
    pfd->Advise(events, &cookie);
    // GetActiveWindow() (not a fixed handle) since this is called both from
    // main menu shortcuts (main window active) and the Browse button inside
    // our own separate dialog SDL window (dialog window active) -- whichever
    // is actually focused when the picker opens becomes its owner, so it's
    // properly modal to (and stays in front of) the right one. This blocks
    // for however long Show() takes (the whole reason this exists) -- but
    // OnFileOk, above, has already fulfilled `promise` well before this
    // returns, so nothing is waiting on this call specifically anymore.
    pfd->Show(GetActiveWindow());
    // Covers cancel (or anything else that closes the dialog without
    // OnFileOk ever firing) -- a no-op if OnFileOk already fulfilled it.
    events->fulfill(std::nullopt);
    pfd->Unadvise(cookie);
    pfd->Release();
  } else {
    events->fulfill(std::nullopt);
  }
  events->Release();
  if (weInitialized) CoUninitialize();
}
#endif  // defined(_WIN32)

constexpr int kScale = 6;         // pixels per dot
// ImGui's default main menu bar height at default font size.
constexpr int kMenuBarHeight = 19;
constexpr int kMarginTop = 20;
constexpr int kMarginBottom = 20;
constexpr int kMarginLeft = 3 * kScale;
constexpr int kMarginRight = 3 * kScale;
constexpr int kWindowW = pc1500::Lcd::kColumns * kScale + kMarginLeft + kMarginRight;

// Fixed-segment status indicator row, shown above the dot matrix (see
// docs/pc1500_hardware_reference.md -- bytes 764EH/764FH, not part of the
// 156x7 graphic display). Laid out left to right per real-hardware
// confirmation from Paul.
constexpr int kIndicatorBarHeight = 18;
constexpr int kIndicatorFontPtSize = 11;
// A .ttc font collection; index picks a language face, but katakana glyphs
// don't differ meaningfully between the CJK variants for our purposes.
// Windows has shipped a CJK-capable font collection by default since
// Windows 8 regardless of display language, so msgothic.ttc needs no
// separate install. macOS and Linux vary more (different macOS releases
// ship different default font sets -- confirmed on real hardware that
// there's no /usr/share/fonts at all on macOS, that's Linux-only FHS
// convention; Linux distros vary in whether/where Noto CJK is installed),
// so those platforms try a short list of known candidate paths in order at
// startup (see the loop around TTF_OpenFontIndex, below) and use the first
// one that actually opens, rather than trusting a single hardcoded guess.
#if defined(_WIN32)
constexpr const char* kIndicatorFontPathCandidates[] = {"C:\\Windows\\Fonts\\msgothic.ttc"};
#elif defined(__APPLE__)
// Verified against a real `ls /System/Library/Fonts` on Paul's Mac
// (2026-08-09) -- Hiragino Sans GB.ttc and the native-named Hiragino Kaku
// Gothic W4.ttc both exist there; PingFang.ttc does NOT exist as a
// top-level file (dropped, was a guess); Apple's Korean font is named
// AppleSDGothicNeo.ttc with no spaces (not "Apple SD Gothic Neo.ttc," the
// initial guess); Arial Unicode.ttf lives under Fonts/Supplemental/, not
// /Library/Fonts.
constexpr const char* kIndicatorFontPathCandidates[] = {
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W4.ttc",  // Hiragino Kaku Gothic (native Japanese font name)
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
};
#else
constexpr const char* kIndicatorFontPathCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
};
#endif
constexpr int kIndicatorFontFaceIndex = 0;

// Status panel below the LCD (ROM/module/CPU details, see its render
// block in main()). This project doesn't enable ImGui's multi-viewport
// mode (it manages its own separate SDL windows for dialogs instead), so
// a MenuItem dropdown taller than the window is clipped to the window's
// own pixel bounds rather than floating as an independent OS window --
// ImGui falls back to its own built-in scroll arrows for the overflow in
// that case, rather than the item simply being unreachable. Kept the
// window at its compact, content-sized default rather than padding it out
// to fit every dropdown: a correctly-proportioned emulator window matters
// more here than never having to scroll a menu.
constexpr int kStatusPanelHeight = 180;

constexpr int kWindowHNoPanel = pc1500::Lcd::kRows * kScale + kMarginTop + kMarginBottom +
                                 kIndicatorBarHeight + kMenuBarHeight;
constexpr int kWindowHWithPanel = kWindowHNoPanel + kStatusPanelHeight;

// The fixed set of strings the indicator row can ever show -- rendered
// once into cached textures at startup rather than every frame.
constexpr const char* kBusy = "BUSY";
constexpr const char* kShift = "SHIFT";
constexpr const char* kKatakana = "\xE3\x82\xAB\xE3\x82\xBF";  // katakana ka-ta
constexpr const char* kSmall = "SMALL";
constexpr const char* kDeg = "DEG";
constexpr const char* kRad = "RAD";
constexpr const char* kGrad = "GRAD";
constexpr const char* kRun = "RUN";
constexpr const char* kPro = "PRO";
constexpr const char* kReserve = "RESERVE";
constexpr const char* kDef = "DEF";
constexpr const char* kOne = "I";
constexpr const char* kTwo = "II";
constexpr const char* kThree = "III";
constexpr const char* kAllIndicatorTexts[] = {
    kBusy, kShift, kKatakana, kSmall, kDeg, kRad, kGrad, kRun, kPro, kReserve, kDef, kOne, kTwo, kThree,
};
// I/II/III are a tight cluster of program-level indicators on real
// hardware (basically one space apart from each other -- confirmed by
// Paul), not spread evenly across the bar like the other indicators, so
// they're laid out separately from kIndicatorSlotCount below.
constexpr int kIndicatorSlotCount = 9;  // left-to-right positions for the other indicators

// Approximate: 2.6MHz crystal / 2 = 1.3MHz internal machine cycle (manual
// section 4-2-1). Not cycle-accurate frame pacing, just a reasonable bring
// -up budget so the emulator runs at roughly the real machine's speed.
constexpr int kCyclesPerSecond = 1300000;
constexpr int kFramesPerSecond = 60;
constexpr int kCyclesPerFrame = kCyclesPerSecond / kFramesPerSecond;

// Buzzer (BASIC BEEP): there's no tone/frequency register on real
// hardware (see bus.h's IoPortController::buzzerOn() comment) -- the
// audible pitch comes entirely from the ROM toggling OPC bit 6 in a
// timed software loop, so emulating it correctly just means sampling
// that bit's live level at audio rate and outputting it directly as a
// square wave. Sampling is done cycle-accurately within each frame's
// CPU-stepping loop (not from a separate SDL audio callback thread)
// specifically to avoid the mismatch between real wall-clock audio
// timing and this emulator's per-frame-batched CPU stepping: an audio
// callback sampling asynchronously would only see whatever the buzzer
// bit happened to be at the moment it ran, missing essentially all of
// a frame's worth of toggling.
//
// How many samples a *given* frame contributes is a separate, dynamic
// question decided at runtime from real elapsed wall-clock time (see
// audioSampleAccumulator in main()) -- kMaxAudioSamplesPerFrame here is
// only a safety cap (covers a ~200ms stall, e.g. a slow startup frame
// or a breakpoint) sizing the per-frame scratch buffer, not the normal
// per-frame sample count.
constexpr int kAudioSampleRate = 48000;
constexpr int kMaxAudioSamplesPerFrame = kAudioSampleRate / 5;
constexpr int16_t kBuzzerAmplitude = 8000;

// DC-blocking (one-pole high-pass) cutoff for the buzzer signal -- see
// buzzerDcEstimate's comment in main(). 20Hz is comfortably below the
// lowest documented BEEP tone (~230Hz, PC1500 Owner's Manual F.1) so real
// tones pass through essentially unaffected, while a level that just sits
// at one static value decays to silence within ~50ms.
constexpr double kBuzzerDcBlockAlpha = 2.0 * 3.14159265358979323846 * 20.0 / kAudioSampleRate;

// Timer tick rate, calibrated 2026-08-02 against the real PC-1500's
// observed edit-cursor blink rate (visible by typing a char then pressing
// Left to move the cursor back over it): Paul timed 10 real-hardware
// blinks at 8 seconds. The old machine-cycle/64 guess (borrowed by
// analogy from the PC-2 manual's crystal/128 divider, itself based on a
// wrong 4MHz-crystal assumption -- both machines actually share the same
// 2.6MHz crystal/1.3MHz CPU clock) produced a visibly too-slow blink
// (measured ~2s/toggle, i.e. ~4s full cycle, 5x too slow). Bisected live
// against the running emulator (64 -> 13 -> 11 -> 10 -> 9 -> 8), each
// step timed the same way: 9 was a hair slow, 8 a hair fast, both within
// stopwatch precision of correct. Chose 8 -- a power-of-two divider is
// far more plausible in real silicon than 9, and it also errs in the
// direction that helps keyboard-dispatch responsiveness (a separate,
// related fix) rather than against it. See
// [[pc1500_keyword_table_mechanism]] for the full process, including a
// machine-cycle/1 dead end that was visibly too fast.
constexpr int kCyclesPerTimerTick = 8;

// SDL keycode -> PC-1500 matrix key. Host physical key state maps directly
// to PC-1500 physical key state (ignoring host shift/modifiers) since the
// PC-1500's own Shift/Sml keys are themselves ordinary matrix keys --
// interpreting Shift+X combinations is the ROM's job, not ours. Where
// there's no natural 1:1 host key, a reasonable nearby substitute is
// picked (documented inline).
//
// **Update, 2026-08-02: "maps directly" is no longer literally true for
// ordinary text-producing keys.** A human typing on the host keyboard can
// release one key and press the next with far less gap than the ROM's
// own per-key dispatch settle time requires (see kIdleFrames's comment
// below) -- confirmed by Paul: typing "PRINT" quickly on the host often
// only registers as "PT" or "PI" in the emulator, while the *exact same*
// timing-controlled synthetic typing (typeImmediateLine, this file) never
// drops a character. Real hardware doesn't have this problem because a
// physical keyboard's natural travel time already exceeds the ROM's own
// requirement; a modern host keyboard/OS event stream can report a
// press/release cadence far faster than that. Fix: ordinary keys
// (letters/digits/space/punctuation) are queued through the same
// fire-and-forget symbolActionQueue mechanism the SDL_TEXTINPUT-driven
// symbol/digit dispatch already uses (see the comment above kTapFrames)
// (see isImmediateHostKey below for the keys deliberately excluded from
// this and kept as direct, zero-latency passthrough instead).
struct KeyMapping {
  SDL_Keycode keycode;
  pc1500::Key key;
};

// Keys that must stay direct/immediate host-state passthrough rather than
// being queued through symbolActionQueue like ordinary text keys (see the
// kKeyMap comment above). Per Paul: cursor keys need to feel live for
// rollover/repeat (already governed by their own, separate
// kCursorRepeatCycles mechanism in Bus::advanceCycles -- queuing them here
// too would fight that), and Cl/Off need to respond instantly (e.g.
// interrupting a running program) rather than sitting behind a queued
// tap's own settle delay. Extended to the other non-text control keys
// (Ent, Shift, Mode, Def, Rcl, F1-F6) on the same reasoning -- none of
// them are part of the "type text quickly" scenario this fix targets, and
// delaying their press edge behind a queue has no upside.
bool isImmediateHostKey(pc1500::Key k) {
  switch (k) {
    case pc1500::Key::Left:
    case pc1500::Key::Right:
    case pc1500::Key::Up:
    case pc1500::Key::Down:
    case pc1500::Key::Cl:
    case pc1500::Key::Off:
    case pc1500::Key::Ent:
    case pc1500::Key::Shift:
    case pc1500::Key::Mode:
    case pc1500::Key::Def:
    case pc1500::Key::Rcl:
    case pc1500::Key::F1:
    case pc1500::Key::F2:
    case pc1500::Key::F3:
    case pc1500::Key::F4:
    case pc1500::Key::F5:
    case pc1500::Key::F6:
      return true;
    default:
      return false;
  }
}

// clang-format off
constexpr KeyMapping kKeyMap[] = {
    // Deliberately no top-row-digit entries (SDLK_0-SDLK_9) here -- SDL's
    // Windows backend hardcodes that scancode range's keycode to always
    // report digits regardless of the OS's actual keyboard layout (a
    // deliberate SDL quirk so number-row game hotkeys stay consistent
    // across layouts), which makes keycode matching useless for
    // distinguishing "1" from AZERTY's unshifted "&" on that same
    // physical key. Numpad digits are unaffected (always genuinely
    // digits, on every layout) and keep their direct entries below; the
    // top row is handled by SDL_TEXTINPUT instead (see the dispatch loop
    // and the comment above kTapFrames), which reports the real composed
    // character on every platform/layout with no special-casing needed.
    {SDLK_KP_0, pc1500::Key::Digit0}, {SDLK_KP_1, pc1500::Key::Digit1},
    {SDLK_KP_2, pc1500::Key::Digit2}, {SDLK_KP_3, pc1500::Key::Digit3},
    {SDLK_KP_4, pc1500::Key::Digit4}, {SDLK_KP_5, pc1500::Key::Digit5},
    {SDLK_KP_6, pc1500::Key::Digit6}, {SDLK_KP_7, pc1500::Key::Digit7},
    {SDLK_KP_8, pc1500::Key::Digit8}, {SDLK_KP_9, pc1500::Key::Digit9},
    {SDLK_a, pc1500::Key::A}, {SDLK_b, pc1500::Key::B}, {SDLK_c, pc1500::Key::C},
    {SDLK_d, pc1500::Key::D}, {SDLK_e, pc1500::Key::E}, {SDLK_f, pc1500::Key::F},
    {SDLK_g, pc1500::Key::G}, {SDLK_h, pc1500::Key::H}, {SDLK_i, pc1500::Key::I},
    {SDLK_j, pc1500::Key::J}, {SDLK_k, pc1500::Key::K}, {SDLK_l, pc1500::Key::L},
    {SDLK_m, pc1500::Key::M}, {SDLK_n, pc1500::Key::N}, {SDLK_o, pc1500::Key::O},
    {SDLK_p, pc1500::Key::P}, {SDLK_q, pc1500::Key::Q}, {SDLK_r, pc1500::Key::R},
    {SDLK_s, pc1500::Key::S}, {SDLK_t, pc1500::Key::T}, {SDLK_u, pc1500::Key::U},
    {SDLK_v, pc1500::Key::V}, {SDLK_w, pc1500::Key::W}, {SDLK_x, pc1500::Key::X},
    {SDLK_y, pc1500::Key::Y}, {SDLK_z, pc1500::Key::Z},
    // Deliberately no {SDLK_MINUS, ...}/{SDLK_EQUALS, ...}/{SDLK_SLASH,
    // ...}/{SDLK_PERIOD, ...} entries -- same class of bug the top-row
    // digits comment above already explains: these are direct-keycode
    // entries that ignore host Shift entirely, and (confirmed on Windows/
    // AZERTY, same root cause as the digit row) the underlying keycode is
    // tied to physical scancode position, not actual layout/shift-level
    // glyph. On AZERTY, the key two right of '0' is unshifted '=',
    // shifted '+' -- but this direct entry always sent plain Equals
    // regardless of Shift, so Shift+that-key produced '=' instead of '+'.
    // charToTapActions already handles '.', '/', '-', '=' (and their
    // shifted forms) correctly by the actual character SDL_TEXTINPUT
    // reports, so removing these entries here lets that layout-aware path
    // handle them, exactly like the digit row.
    {SDLK_LEFTBRACKET, pc1500::Key::LeftParen},   // nearest substitute for (
    {SDLK_RIGHTBRACKET, pc1500::Key::RightParen}, // nearest substitute for )
    {SDLK_KP_PLUS, pc1500::Key::Plus},
    {SDLK_KP_MULTIPLY, pc1500::Key::Asterisk},
    {SDLK_LEFT, pc1500::Key::Left}, {SDLK_RIGHT, pc1500::Key::Right},
    {SDLK_UP, pc1500::Key::Up}, {SDLK_DOWN, pc1500::Key::Down},
    // Deliberately no {SDLK_LSHIFT, ...} or {SDLK_RSHIFT, ...} entry --
    // both host Shift keys behave identically, as pure modifiers, never
    // mapped directly to any PC-1500 key. Holding PC-1500 Shift
    // continuously for the whole time a host Shift is down means the
    // ROM's key-scan sees "Shift alone" as its own key event *before*
    // whatever other key is being combined with it -- and, just like
    // the MODE/CL debounce we traced earlier, that latches a ~0.2s "a
    // key was just processed" window that swallows the very next key
    // event (the actual target key), which arrives well within normal
    // Shift+key typing speed. So host Shift (either one) is read only
    // as a modifier (event.key.keysym.mod) and only ever engages
    // PC-1500 Shift atomically together with a specific target key, via
    // the SDL_TEXTINPUT-driven dispatch below. For non-printable keys
    // that doesn't cover, Tab sends a direct, standalone PC-1500 Shift
    // keypress (real hardware's own
    // Shift is a tap-to-toggle key, not a hold, so this matches that
    // directly rather than needing the tap-sequence machinery below).
    {SDLK_TAB, pc1500::Key::Shift},
    {SDLK_BACKSPACE, pc1500::Key::Left},           // duplicates the left arrowhead
    {SDLK_RETURN, pc1500::Key::Ent},               // the *small* Ent key -- the
                                                    // main ENTER key's matrix
                                                    // position was never
                                                    // located (see hardware
                                                    // reference doc)
    {SDLK_SPACE, pc1500::Key::Space},
    {SDLK_F1, pc1500::Key::F1}, {SDLK_F2, pc1500::Key::F2},
    {SDLK_F3, pc1500::Key::F3}, {SDLK_F4, pc1500::Key::F4},
    {SDLK_F5, pc1500::Key::F5}, {SDLK_F6, pc1500::Key::F6},
    {SDLK_F7, pc1500::Key::Cl}, {SDLK_F8, pc1500::Key::Mode},
    {SDLK_F9, pc1500::Key::Def}, {SDLK_F11, pc1500::Key::Rcl},
};
// clang-format on

// F10 and F12 need host-modifier awareness (Shift/Ctrl), so they're
// handled specially rather than through the plain kKeyMap table:
//   F10          -> Sml
//   Shift+F10    -> the up/down rocker key
//   F12          -> On (not part of the 8x8 matrix -- wired straight to
//                  the CPU's BFI pin, so it's handled separately from
//                  every other key, same as on real hardware)
//   Shift+F12    -> Off
//   Ctrl+F12     -> host-only convenience key mimicking the real
//                  machine's recessed ALL RESET pinhole switch (also not
//                  part of the matrix) -- re-runs CPU::reset() without
//                  otherwise touching RAM.

// Punctuation/symbol passthrough: on the real PC-1500, typing these
// requires PC-1500 Shift with a specific matrix key (confirmed by Paul
// on real hardware) -- e.g. Shift+F6 always types '&' regardless of
// whether F6 has been separately programmed as a command shortcut, same
// as Shift+/ always types '?'. Pass Shift+F6 as PC-1500 Shift+F6 and
// plain F6 as plain F6, and let the ROM decide what to do -- don't try
// to be clever about current RUN/PRO mode here.
//
// Crucially, PC-1500 Shift is a *toggle*, not a hold: on real hardware
// you tap Shift (press and release), which lights the Shift indicator
// indefinitely (no timeout) until Shift or any other key is pressed,
// and that next keypress is shifted. Shift is never held down
// simultaneously with another key. Simulating a hold instead (as a
// modern keyboard's Shift works) breaks things: the ROM's key-scan
// would see "Shift alone" as its own key event the instant host Shift
// goes down, *before* the target key arrives, which used to latch a
// ~0.2s "a key was just processed" debounce window we traced for
// MODE/CL -- well within normal Shift+key typing speed, so it swallowed
// the target key's press entirely.
//
// **Update, 2026-08-02: the real root cause was found and fixed --
// kCyclesPerTimerTick (below) was wrong by 8x.** The ROM's own 7B0EH
// key-dispatch gate fix (linear -> real 9-bit polynomial timer, see
// lh5801_timer_table.h) was necessary but not sufficient: it fixed
// BASWORD's resident keyboard-hook, but a second ~230ms cliff remained in
// kIdleFrames (14 frames registered reliably, 13 failed reliably) that
// didn't budge. That ~230ms turned out to be the real ROM's own 8-period
// debounce countdown (E260H-E370H in ROM1.BIN, called from BASIC's
// keyboard-polling mainline at DC7BH) running at the WRONG rate, because
// kCyclesPerTimerTick was carrying an unconfirmed guess (borrowed from
// the PC-2 manual's crystal/128 divider, itself based on a wrong
// 4MHz-crystal assumption for the PC-2 -- both machines actually share
// the same 2.6MHz crystal/1.3MHz CPU clock). Calibrated live against
// Paul's real-hardware edit-cursor blink-rate measurement (10 blinks in
// 8 seconds) instead: the correct divider is 8, not 64. With that fixed,
// kIdleFrames could finally shrink for real -- see its own comment below.
// See [[pc1500_keyword_table_mechanism]] in memory for the full
// calibration process.
//
// So each symbol is fired as a queued, fire-and-forget sequence of
// PC-1500 key actions (see QueuedKeyAction/symbolActionQueue below),
// entirely decoupled from how long the *host* key is actually held:
//   1. Press PC-1500 Shift, hold for kTapFrames (long enough for one
//      ROM scan cycle to see it as its own event).
//   2. Release Shift, then wait *idle* (nothing pressed) for
//      kIdleFrames -- see the empirical cliff described above; this is
//      not a "one scan period" margin, there's a real, larger constraint
//      still not fully understood.
//   3. Press the target key, hold for kTapFrames, then release.
// Firing this as a queue (rather than tracking per-key held/released
// state) also fixes an earlier, more serious bug: since a normal human
// keypress is much shorter than the ~0.3s this sequence takes, tying it
// to "is the host key still held" meant the sequence was almost always
// cancelled partway through, before the target key was ever pressed --
// which is why symbols "mostly didn't work" before. Once queued, a
// press is committed and runs to completion regardless of when the
// host key is released; OS key-repeat while a host key is held is
// filtered out (event.key.repeat) so a long hold doesn't flood the
// queue with duplicate sequences.
//
// **Update, 2026-08-04: what actually decides which symbol/digit fires
// switched from SDL_Keycode to SDL_TEXTINPUT.** The original design (see
// git history for the removed kSymbolMap/kShiftedDirectMap tables) keyed
// symbols off SDL_Keycode + a host-Shift flag, hardcoding the QWERTY
// assumption that (say) Shift+SDLK_1 means "!" and plain SDLK_1 means
// "1". That's wrong on any layout where the number row's *un*shifted
// glyph isn't a digit (AZERTY and most other European layouts put digits
// on the Shift layer) -- confirmed as a real bug by Paul on Windows/
// AZERTY, and independently confirmed empirically: SDL's Windows backend
// hardcodes the top-row scancodes' keycode to always report SDLK_1..
// SDLK_0 regardless of the OS's real keyboard layout (deliberately, so
// number-row game hotkeys stay consistent across layouts), so keycode
// matching can't even see AZERTY's real "&" there in the first place.
// SDL_TEXTINPUT sidesteps this entirely: it reports the actual composed
// character for *any* platform/layout (it's literally the OS's own text
// composition, the same source a text editor would use), so the same
// charToTapActions(char, ...) function that already drives the
// scriptable command interface's `type` command (see below) now drives
// live keyboard symbol/digit input too -- one character-indexed
// implementation instead of two parallel (and, on Windows, differently
// broken) keycode-indexed tables. This is why the top-row digit entries
// are gone from kKeyMap above: SDL_TEXTINPUT is now the *only* source for
// that scancode range, both for its shifted (digit) and unshifted
// (symbol) meaning. See the SDL_TEXTINPUT branch in the event dispatch
// loop below, and suppressNextTextInput's comment for how it avoids
// double-handling keys kKeyMap already deals with directly (letters,
// numpad, arrows, etc.).
// kTapFrames (~67ms, spans the ROM's ~25ms scan cadence) / kIdleFrames
// (~67ms, empirically bisected minimum that still reliably registers
// BASWORD keywords -- 3 fails reliably) and QueuedKeyAction (one step of a
// queued symbol-tap sequence: set a PC-1500 key's state, then wait
// `framesToWait` real frames before the next queued action runs, processed
// one action at a time in the main loop, independent of host key state --
// the "fire-and-forget" rationale below) now live in text_loader.h, pulled
// into scope by the `using` block above.

// ---- Scriptable command interface (pc1500emu.cmd) ----
//
// Lets an external process (Claude, via Bash) drive a *running* emulator
// session directly -- type text, press named keys, peek/poke memory, dump
// the display -- instead of relaying everything through a human typing on
// the actual keyboard and reporting results back by hand. Query commands
// (peek/dump/status/display) write their result to a response file
// (kResponsePath) for the caller to read back afterward.
//
// Deliberately simple/textual (one command per line). On POSIX this is a
// real FIFO at a fixed /tmp path, writable with plain shell redirection
// (`echo cmd > path`). Windows has no filesystem-visible equivalent, so
// there it's a named pipe instead (see the cmdPipe* functions below) --
// write to it with tools/send-command.ps1 rather than shell redirection.
#if defined(_WIN32)
constexpr const char* kCommandPipeName = "\\\\.\\pipe\\pc1500emu.cmd";
#else
constexpr const char* kCommandFifoPath = "/tmp/pc1500emu.cmd";
#endif

// Returns a path for `filename` in the platform temp directory: a fixed
// /tmp on POSIX, or the directory %TEMP% (etc.) resolves to on Windows.
std::string platformTempFilePath(const char* filename) {
#if defined(_WIN32)
  char dir[MAX_PATH]{};
  DWORD n = GetTempPathA(static_cast<DWORD>(sizeof(dir)), dir);
  std::string path = (n > 0 && n < sizeof(dir)) ? std::string(dir, n) : std::string("C:\\Windows\\Temp\\");
  if (!path.empty() && path.back() != '\\') path += '\\';
  return path + filename;
#else
  return std::string("/tmp/") + filename;
#endif
}

// Forces `window`'s already-Present()'d frame to actually be composited to
// screen immediately, rather than waiting for whatever normally triggers a
// repaint (focus change, resize, alt-tab...). Needed for the dialog
// window's periodic "Loading..." repaints during a long blocking call --
// see typeBasicProgramText's onProgress callback, below -- since
// SDL_RenderPresent alone submits the frame to the swap chain, and even
// SDL_PumpEvents pumping the message queue isn't enough on Windows to make
// DWM actually paint it: confirmed, without this, the dialog visibly only
// updated once the window happened to lose and regain focus. RedrawWindow
// with RDW_UPDATENOW forces a synchronous WM_PAINT dispatch right here
// rather than leaving it queued.
void forceWindowRepaint(SDL_Window* window) {
#if defined(_WIN32)
  SDL_SysWMinfo wmInfo;
  SDL_VERSION(&wmInfo.version);
  if (SDL_GetWindowWMInfo(window, &wmInfo)) {
    RedrawWindow(wmInfo.info.win.window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
  }
#else
  (void)window;
#endif
}

const std::string kResponsePath = platformTempFilePath("pc1500emu.response");

#if defined(_WIN32)
// Win32 named-pipe equivalent of the POSIX FIFO. Uses overlapped
// (asynchronous) I/O throughout so polling it every frame never blocks the
// GUI loop -- ConnectNamedPipe/ReadFile both return immediately and
// completion is checked via GetOverlappedResult(..., bWait=FALSE), mirroring
// the O_NONBLOCK FIFO read on POSIX.
HANDLE g_cmdPipe = INVALID_HANDLE_VALUE;
OVERLAPPED g_cmdPipeOverlapped{};
bool g_cmdPipeConnected = false;
bool g_cmdPipeIoPending = false;
char g_cmdPipeReadBuf[4096];

bool cmdPipeInit(const char* pipeName) {
  g_cmdPipeOverlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  g_cmdPipe = CreateNamedPipeA(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                4096, 4096, 0, nullptr);
  return g_cmdPipe != INVALID_HANDLE_VALUE;
}

// Drops the current client and re-arms the pipe to accept the next one --
// each send-command.ps1 invocation is a short-lived connect/write/close, so
// this runs after essentially every command.
void cmdPipeReset() {
  DisconnectNamedPipe(g_cmdPipe);
  g_cmdPipeConnected = false;
  g_cmdPipeIoPending = false;
}

// Called once per frame; appends any newly-arrived bytes to *out.
void cmdPipePoll(std::string* out) {
  if (g_cmdPipe == INVALID_HANDLE_VALUE) return;

  if (!g_cmdPipeConnected) {
    if (!g_cmdPipeIoPending) {
      ResetEvent(g_cmdPipeOverlapped.hEvent);
      if (ConnectNamedPipe(g_cmdPipe, &g_cmdPipeOverlapped)) {
        g_cmdPipeConnected = true;
      } else {
        switch (GetLastError()) {
          case ERROR_PIPE_CONNECTED:
            g_cmdPipeConnected = true;
            break;
          case ERROR_IO_PENDING:
            g_cmdPipeIoPending = true;
            break;
          default:
            return;  // real error -- try again next frame
        }
      }
    } else {
      DWORD transferred;
      if (GetOverlappedResult(g_cmdPipe, &g_cmdPipeOverlapped, &transferred, FALSE)) {
        g_cmdPipeConnected = true;
        g_cmdPipeIoPending = false;
      } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
        g_cmdPipeIoPending = false;  // connect attempt failed -- retry next frame
      }
    }
    if (!g_cmdPipeConnected) return;
  }

  for (;;) {
    if (!g_cmdPipeIoPending) {
      ResetEvent(g_cmdPipeOverlapped.hEvent);
      BOOL ok = ReadFile(g_cmdPipe, g_cmdPipeReadBuf, sizeof(g_cmdPipeReadBuf), nullptr, &g_cmdPipeOverlapped);
      if (!ok) {
        if (GetLastError() == ERROR_IO_PENDING) {
          g_cmdPipeIoPending = true;
        } else {
          cmdPipeReset();  // client disconnected, or a real error
        }
        return;
      }
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(g_cmdPipe, &g_cmdPipeOverlapped, &transferred, FALSE)) {
      if (GetLastError() == ERROR_IO_INCOMPLETE) return;  // read still pending
      g_cmdPipeIoPending = false;
      cmdPipeReset();
      return;
    }
    g_cmdPipeIoPending = false;
    if (transferred == 0) {
      cmdPipeReset();
      return;
    }
    out->append(g_cmdPipeReadBuf, transferred);
  }
}

void cmdPipeClose() {
  if (g_cmdPipe != INVALID_HANDLE_VALUE) {
    CancelIo(g_cmdPipe);
    CloseHandle(g_cmdPipe);
    g_cmdPipe = INVALID_HANDLE_VALUE;
  }
  if (g_cmdPipeOverlapped.hEvent) {
    CloseHandle(g_cmdPipeOverlapped.hEvent);
    g_cmdPipeOverlapped.hEvent = nullptr;
  }
}
#endif  // _WIN32

// charToTapActions (maps a plain typed character to a direct PC-1500
// keypress or a Shift-tap sequence, returning false if there's no mapping
// at all, in which case the caller should use `key NAME` instead) now
// lives in text_loader.h/.cpp, pulled into scope by the `using` block
// above -- the single source of truth for "what does this character do on
// the PC-1500", used both by the scriptable command interface's `type`
// command and (see the SDL_TEXTINPUT branch in the main loop) live
// keyboard input.

// Maps a `key NAME` command's name (case-insensitive) to a PC-1500 key for
// a direct tap -- covers keys with no natural printable-character form
// (Enter, Cl, Mode, ...) plus a few redundant-but-harmless aliases.
bool nameToKey(std::string name, pc1500::Key* out) {
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::pair<const char*, pc1500::Key> kNames[] = {
      {"enter", pc1500::Key::Ent},   {"ent", pc1500::Key::Ent},
      {"cl", pc1500::Key::Cl},       {"mode", pc1500::Key::Mode},
      {"def", pc1500::Key::Def},     {"sml", pc1500::Key::Sml},
      {"rcl", pc1500::Key::Rcl},     {"shift", pc1500::Key::Shift},
      {"off", pc1500::Key::Off},     {"up", pc1500::Key::Up},
      {"down", pc1500::Key::Down},   {"left", pc1500::Key::Left},
      {"right", pc1500::Key::Right}, {"space", pc1500::Key::Space},
      {"f1", pc1500::Key::F1},       {"f2", pc1500::Key::F2},
      {"f3", pc1500::Key::F3},       {"f4", pc1500::Key::F4},
      {"f5", pc1500::Key::F5},       {"f6", pc1500::Key::F6},
  };
  for (const auto& [n, k] : kNames) {
    if (name == n) {
      *out = k;
      return true;
    }
  }
  // Plain single letters/digits, for precise presskey/releasekey timing
  // control in scripted tests (added 2026-08-02 -- see
  // [[pc1500_keyword_table_mechanism]] for why this was needed: without
  // it, `presskey <letter>` silently no-ops, which had been invalidating
  // several earlier isolated-key timing measurements this session).
  if (name.size() == 1) {
    char c = name[0];
    if (c >= 'a' && c <= 'z') {
      *out = static_cast<pc1500::Key>(static_cast<int>(pc1500::Key::A) + (c - 'a'));
      return true;
    }
    if (c >= '0' && c <= '9') {
      *out = static_cast<pc1500::Key>(static_cast<int>(pc1500::Key::Digit0) + (c - '0'));
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ---- ML/binary load-save: CLOAD M / CSAVE M semantics (PC-1500 Technical
// Reference Manual) -- a plain [addr, addr+len) byte range plus a filename,
// no BASIC-state awareness at all (that's BASIC load/save's job, see
// loadBasicProgram/saveBasicProgram below). The real CLOAD M/CSAVE M take
// an explicit address (and, for CSAVE M, length) argument for exactly this
// reason -- unlike a BASIC program, a hand-assembled ML routine has no
// self-describing bounds the machine could infer on its own.
bool loadBinary(pc1500::Bus& bus, uint16_t addr, const char* path, std::string* error) {
  std::vector<uint8_t> data = readFile(path);
  if (data.empty()) {
    *error = "Could not read file (or file is empty).";
    return false;
  }
  bus.loadME0(addr, data.data(), data.size());
  return true;
}

// CE-150/153/158-style plug-in ROM module at 0x8000H-0xBFFFH -- see
// Bus::RomModule's own comment for what slot/base/requirePv/usePuBank mean.
bool loadRomModule(pc1500::Bus& bus, int slot, uint16_t base, bool requirePv, bool usePuBank,
                    const char* path, std::string* error) {
  std::vector<uint8_t> data = readFile(path);
  if (data.empty()) {
    *error = "Could not read file (or file is empty).";
    return false;
  }
  if (usePuBank && (data.size() % 2) != 0) {
    *error = "PU-banked module ROM must have an even size (split into two equal banks).";
    return false;
  }
  bus.loadRomModule(slot, data.data(), data.size(), base, requirePv, usePuBank);
  return true;
}

// Like loadRomModule, but for a module with a genuinely writable data
// window (e.g. the PC1500-PSOC5 board) -- see Bus::RomModule's own comment.
// `path` should be just the ROM bytes (e.g. a trimmed, base-aligned binary),
// not a full window-sized image with the data-window portion blanked out --
// the data window is separately zero/0xFF-initialized, not loaded from a
// file. `sdDir`, if non-empty, points the module's ExpansionMock at a real
// host directory for its SD-card commands (see ExpansionMock::setRootDir);
// omitted/empty leaves it unconfigured ("no card inserted", every SD
// command reports an error). Not validated as existing here -- a
// nonexistent/inaccessible directory just makes every SD command fail at
// call time, which is itself a legitimate state to be able to test.
bool loadExpansionModule(pc1500::Bus& bus, int slot, uint16_t base, bool requirePv,
                          bool usePuBank, uint16_t dataWindowBase, uint16_t dataWindowSize,
                          uint16_t instructionAddr, const char* path, const std::string& sdDir,
                          std::string* error) {
  std::vector<uint8_t> data = readFile(path);
  if (data.empty()) {
    *error = "Could not read file (or file is empty).";
    return false;
  }
  if (usePuBank && (data.size() % 2) != 0) {
    *error = "PU-banked module ROM must have an even size (split into two equal banks).";
    return false;
  }
  if (instructionAddr < dataWindowBase || instructionAddr >= dataWindowBase + dataWindowSize) {
    *error = "instructionAddr must fall within [dataWindowBase, dataWindowBase+dataWindowSize).";
    return false;
  }
  bus.loadExpansionModule(slot, data.data(), data.size(), base, requirePv, usePuBank,
                           dataWindowBase, dataWindowSize, instructionAddr);
  if (!sdDir.empty()) bus.expansionMock().setRootDir(sdDir);
  return true;
}

bool saveBinary(pc1500::Bus& bus, uint16_t addr, uint32_t len, const char* path, std::string* error) {
  if (len == 0) {
    *error = "Length must be greater than 0.";
    return false;
  }
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    *error = "Could not open file for writing.";
    return false;
  }
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b = bus.readME0(static_cast<uint16_t>(addr + i));
    f.put(static_cast<char>(b));
  }
  return true;
}

// BASIC program load/save helpers (kBasicProgramStart, kProgramEndPointerAddr,
// findBasicProgramEnd, readBasicProgramBytes, saveBasicProgram,
// saveBasicTextFile, loadBasicProgram, typeBasicProgramText -- CLOAD/CSAVE
// filename-only semantics, the ROM's program storage format per Technical
// Reference Manual section 5-3-5, and keystroke-driven text loading with
// SML/lowercase handling) now live in src/basic/text_loader.h/.cpp, pulled
// into scope by the 'using' block above.

// Types a single line via direct cycle-stepping (same technique as
// typeBasicProgramText -- reliable regardless of real-frame pacing) and
// presses Enter, without typeBasicProgramText's NEW0/full-replace
// preamble. For driving one-off immediate-mode commands (e.g. testing a
// BASWORD registration command) where wiping the program area first
// isn't wanted.
bool typeImmediateLine(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& line,
                        std::string* error, bool pressEnter = true) {
  int cyclesSinceTimerTick = 0;
  auto stepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  };
  auto runKeyAction = [&](const QueuedKeyAction& action) {
    bus.setKeyState(action.key, action.pressed);
    stepCycles(static_cast<long>(action.framesToWait) * kCyclesPerFrame);
  };
  pc1500::basic::SmlAwareTyper smlTyper(bus);
  for (char c : line) {
    std::deque<QueuedKeyAction> actions;
    if (!smlTyper.typeChar(c, &actions)) {
      *error = "no keystroke mapping for character '" + std::string(1, c) + "'";
      return false;
    }
    for (const QueuedKeyAction& action : actions) runKeyAction(action);
  }
  if (pressEnter) {
    runKeyAction({pc1500::Key::Ent, true, kTapFrames});
    runKeyAction({pc1500::Key::Ent, false, kIdleFrames});
  }
  return true;
}

// Diagnostic variant of typeImmediateLine: every character uses the
// normal kIdleFrames wait after release EXCEPT the character at
// `shortIdlePos` (0-based index into `line`; -1 = none), which uses
// `shortIdleFrames` instead. Enter always uses the normal kIdleFrames.
// For isolating *which* keystroke transition is sensitive to a short
// idle gap, rather than just bisecting the uniform value -- see
// [[pc1500_keyword_table_mechanism]] for why this matters (kIdleFrames
// has a sharp 13-vs-14-frame cliff not explained by the already-fixed
// 7B0EH gate).
bool typeImmediateLinePartialIdle(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& line,
                                   int shortIdlePos, int shortIdleFrames, std::string* error) {
  int cyclesSinceTimerTick = 0;
  auto stepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  };
  auto runKeyAction = [&](const QueuedKeyAction& action) {
    bus.setKeyState(action.key, action.pressed);
    stepCycles(static_cast<long>(action.framesToWait) * kCyclesPerFrame);
  };
  pc1500::basic::SmlAwareTyper smlTyper(bus);
  for (size_t idx = 0; idx < line.size(); idx++) {
    std::deque<QueuedKeyAction> actions;
    if (!smlTyper.typeChar(line[idx], &actions)) {
      *error = "no keystroke mapping for character '" + std::string(1, line[idx]) + "'";
      return false;
    }
    int idleOverride = (shortIdlePos == -2 || static_cast<int>(idx) == shortIdlePos) ? shortIdleFrames : kIdleFrames;
    for (size_t ai = 0; ai < actions.size(); ai++) {
      QueuedKeyAction action = actions[ai];
      // Only the FINAL release action in this character's sequence is the
      // "idle gap before the next character" -- override just that one
      // (SmlAwareTyper may have prepended an SML-toggle pair before this
      // character's own actions, but those always come first, never last,
      // so this still correctly targets only the character's own release).
      if (!action.pressed && ai + 1 == actions.size()) action.framesToWait = idleOverride;
      runKeyAction(action);
    }
  }
  runKeyAction({pc1500::Key::Ent, true, kTapFrames});
  runKeyAction({pc1500::Key::Ent, false, kIdleFrames});
  return true;
}

// Same as typeImmediateLinePartialIdle, but starts instruction tracing
// right after shortIdlePos's character is released (i.e. for the short
// gap itself and the following `tailChars` characters), to directly
// observe what happens during/after the sensitive transition.
bool typeImmediateLinePartialIdleTraced(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& line,
                                         int shortIdlePos, int shortIdleFrames, int tailChars,
                                         long extraCycles,
                                         std::string* error, std::string* traceOut) {
  int cyclesSinceTimerTick = 0;
  std::ostringstream out;
  // shortIdlePos == -1: trace from the very first character's first action,
  // not just from after some later character finishes -- needed to see
  // what happens on the *first* keystroke of a line, not only later ones.
  bool tracing = (shortIdlePos == -1);
  int charsTracedSince = 0;
  auto stepCyclesMaybeTraced = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      uint16_t pBefore = tracing ? cpu.p() : 0;
      uint8_t op = tracing ? bus.readME0(pBefore) : 0;
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
      if (tracing) {
        out << std::hex << std::uppercase;
        out.width(4); out.fill('0'); out << pBefore << " ";
        out.width(2); out.fill('0'); out << static_cast<int>(op) << " A=";
        out.width(2); out.fill('0'); out << static_cast<int>(cpu.a()) << " X=";
        out.width(4); out.fill('0'); out << cpu.x() << " Y=";
        out.width(4); out.fill('0'); out << cpu.y() << " U=";
        out.width(4); out.fill('0'); out << cpu.u() << " S=";
        out.width(4); out.fill('0'); out << cpu.s() << " 7B0E=";
        out.width(2); out.fill('0'); out << static_cast<int>(bus.readME0(0x7B0E)) << "\n";
      }
    }
  };
  auto runKeyAction = [&](const QueuedKeyAction& action) {
    bus.setKeyState(action.key, action.pressed);
    stepCyclesMaybeTraced(static_cast<long>(action.framesToWait) * kCyclesPerFrame);
  };
  pc1500::basic::SmlAwareTyper smlTyper(bus);
  for (size_t idx = 0; idx < line.size(); idx++) {
    std::deque<QueuedKeyAction> actions;
    if (!smlTyper.typeChar(line[idx], &actions)) {
      *error = "no keystroke mapping for character '" + std::string(1, line[idx]) + "'";
      return false;
    }
    int idleOverride = (shortIdlePos == -2 || static_cast<int>(idx) == shortIdlePos) ? shortIdleFrames : kIdleFrames;
    for (size_t ai = 0; ai < actions.size(); ai++) {
      QueuedKeyAction action = actions[ai];
      if (!action.pressed && ai + 1 == actions.size()) action.framesToWait = idleOverride;
      runKeyAction(action);
    }
    if (static_cast<int>(idx) == shortIdlePos) tracing = true;
    if (tracing) {
      charsTracedSince++;
      if (charsTracedSince > tailChars) break;
    }
  }
  // Keep tracing past the last typed character's own kIdleFrames settle --
  // some display/state updates (e.g. the LCD buffer mode byte at 7880H)
  // don't happen within a single character's own minimal settle window,
  // only after further natural idle-loop cycling.
  if (extraCycles > 0) {
    tracing = true;
    stepCyclesMaybeTraced(extraCycles);
  }
  *traceOut = out.str();
  return true;
}

// Debug-only watchpoint: types `line` (same proven cycle-stepping path as
// typeImmediateLine, from the very first character), then continues for
// `extraCycles` more, and after EVERY single instruction checks whether
// the `watchLen` bytes starting at `watchAddr` changed since the previous
// instruction. Since exactly one instruction executes between checks, any
// change is unambiguously attributable to the instruction at `pBefore` --
// far faster than eyeballing full instruction traces to find which of
// thousands of instructions touched a specific address. Logs one line per
// write: PC, old bytes, new bytes, plus A/X/Y/U for context.
bool typeImmediateLineWatch(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& line,
                             uint16_t watchAddr, int watchLen, long extraCycles, std::string* error,
                             std::string* watchOut) {
  int cyclesSinceTimerTick = 0;
  std::ostringstream out;
  std::vector<uint8_t> prevBytes(watchLen);
  for (int i = 0; i < watchLen; i++) prevBytes[i] = bus.readME0(static_cast<uint16_t>(watchAddr + i));
  auto stepCyclesWatched = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      uint16_t pBefore = cpu.p();
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
      bool changed = false;
      for (int b = 0; b < watchLen; b++) {
        if (bus.readME0(static_cast<uint16_t>(watchAddr + b)) != prevBytes[b]) changed = true;
      }
      if (changed) {
        out << std::hex << std::uppercase;
        out.width(4); out.fill('0'); out << pBefore << " wrote " << watchAddr << ": ";
        for (int b = 0; b < watchLen; b++) {
          out.width(2); out.fill('0'); out << static_cast<int>(prevBytes[b]) << " ";
        }
        out << "-> ";
        for (int b = 0; b < watchLen; b++) {
          uint8_t nv = bus.readME0(static_cast<uint16_t>(watchAddr + b));
          out.width(2); out.fill('0'); out << static_cast<int>(nv) << " ";
          prevBytes[b] = nv;
        }
        out << " A="; out.width(2); out.fill('0'); out << static_cast<int>(cpu.a());
        out << " X="; out.width(4); out.fill('0'); out << cpu.x();
        out << " Y="; out.width(4); out.fill('0'); out << cpu.y();
        out << " U="; out.width(4); out.fill('0'); out << cpu.u();
        out << " S="; out.width(4); out.fill('0'); out << cpu.s() << "\n";
      }
    }
  };
  auto runKeyAction = [&](const QueuedKeyAction& action) {
    bus.setKeyState(action.key, action.pressed);
    stepCyclesWatched(static_cast<long>(action.framesToWait) * kCyclesPerFrame);
  };
  pc1500::basic::SmlAwareTyper smlTyper(bus);
  for (char c : line) {
    std::deque<QueuedKeyAction> actions;
    if (!smlTyper.typeChar(c, &actions)) {
      *error = "no keystroke mapping for character '" + std::string(1, c) + "'";
      return false;
    }
    for (const QueuedKeyAction& action : actions) runKeyAction(action);
  }
  if (extraCycles > 0) stepCyclesWatched(extraCycles);
  *watchOut = out.str();
  return true;
}

// Same proven typing path as typeImmediateLine, but the Enter press,
// its release/settle wait, and traceCycles beyond that are ALL traced as
// one continuous window -- not just the cycles after Enter's settle
// period. Earlier versions of this traced only after the settle delay
// and missed fast-resolving dispatches entirely (confirmed: a command
// that fully executes within Enter's own kTapFrames+kIdleFrames settle,
// e.g. PRINT 1+1, showed the correct result on screen but zero relevant
// addresses in the trace -- the interesting execution had already
// finished before tracing started). Typing itself (before Enter) is
// still untraced, on the same proven code path as typeImmediateLine.
bool typeImmediateLineWithTrace(pc1500::Bus& bus, lh5801::CPU& cpu, const std::string& line,
                                 long traceCycles, std::string* error, std::string* traceOut) {
  int cyclesSinceTimerTick = 0;
  auto stepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }
  };
  auto runKeyAction = [&](const QueuedKeyAction& action) {
    bus.setKeyState(action.key, action.pressed);
    stepCycles(static_cast<long>(action.framesToWait) * kCyclesPerFrame);
  };
  pc1500::basic::SmlAwareTyper smlTyper(bus);
  for (char c : line) {
    std::deque<QueuedKeyAction> actions;
    if (!smlTyper.typeChar(c, &actions)) {
      *error = "no keystroke mapping for character '" + std::string(1, c) + "'";
      return false;
    }
    for (const QueuedKeyAction& action : actions) runKeyAction(action);
  }
  std::ostringstream out;
  auto traceStepCycles = [&](long cycles) {
    for (long i = 0; i < cycles;) {
      uint16_t pBefore = cpu.p();
      uint8_t op = bus.readME0(pBefore);
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      i += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
      out << std::hex << std::uppercase;
      out.width(4); out.fill('0'); out << pBefore << " ";
      out.width(2); out.fill('0'); out << static_cast<int>(op) << " A=";
      out.width(2); out.fill('0'); out << static_cast<int>(cpu.a()) << " X=";
      out.width(4); out.fill('0'); out << cpu.x() << " Y=";
      out.width(4); out.fill('0'); out << cpu.y() << " U=";
      out.width(4); out.fill('0'); out << cpu.u() << "\n";
    }
  };
  bus.setKeyState(pc1500::Key::Ent, true);
  traceStepCycles(static_cast<long>(kTapFrames) * kCyclesPerFrame);
  bus.setKeyState(pc1500::Key::Ent, false);
  traceStepCycles(static_cast<long>(kIdleFrames) * kCyclesPerFrame);
  long i = 0;
  while (i < traceCycles) {
    uint16_t pBefore = cpu.p();
    uint8_t op = bus.readME0(pBefore);
    int c = cpu.step();
    int used = (c > 0) ? c : 1;
    i += used;
    cyclesSinceTimerTick += used;
    bus.advanceCycles(used);
    bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
    while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      cpu.tickTimer();
      cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
    out << std::hex << std::uppercase;
    out.width(4); out.fill('0'); out << pBefore << " ";
    out.width(2); out.fill('0'); out << static_cast<int>(op) << " A=";
    out.width(2); out.fill('0'); out << static_cast<int>(cpu.a()) << " X=";
    out.width(4); out.fill('0'); out << cpu.x() << " Y=";
    out.width(4); out.fill('0'); out << cpu.y() << " U=";
    out.width(4); out.fill('0'); out << cpu.u() << "\n";
  }
  *traceOut = out.str();
  return true;
}

void printUsage(const char* argv0) {
  std::printf(
      "Usage: %s [options] [<rom.bin>]\n"
      "\n"
      "  <rom.bin>            Path to the PC-1500 system ROM to load at 0xC000\n"
      "                       (16KB). Optional if a --conf file specifies its own\n"
      "                       romPath; a path given here always overrides that.\n"
      "  --conf <path>        Load settings (ROM path, state-file path, auto-load/\n"
      "                       auto-save preferences) from a JSON conf file. See\n"
      "                       README.md's state save/load section for the schema.\n"
      "  --no-state           Skip auto-loading the configured state file this run\n"
      "                       (even if Auto-Load State on Start is enabled), and\n"
      "                       boot cold instead -- for testing/reproduction runs\n"
      "                       that need to start from a known, empty state rather\n"
      "                       than wherever a previous session left off. Does not\n"
      "                       change the Auto-Load setting itself, and does not\n"
      "                       affect Auto-Save on Exit for this run.\n"
      "  -h, --help, /h, /?   Show this help and exit.\n",
      argv0);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help" || arg == "/h" || arg == "/?") {
      printUsage(argv[0]);
      return 0;
    }
  }


  pc1500::Keyboard keyboard;
  pc1500::Bus bus(keyboard);
  lh5801::CPU cpu(bus);
  pc1500::Lcd lcd;

  // Sync the uPD1990AC RTC to the host's current time up front, the same
  // effect BASIC's own TIME command would have, so TIME$/DATE$ read
  // correctly from the very first ROM boot without the user needing to
  // run TIME by hand -- matching a real PC-1500's battery-backed RTC,
  // which would already show correct time on power-up.
  bus.ioPort().syncRtcToHostClock();

  // Command-line: `--conf <path>` may appear anywhere; the first other
  // token is the positional ROM path (unchanged from before this feature
  // existed). A given positional ROM path always wins over the conf
  // file's own romPath -- the more immediate/explicit signal takes
  // precedence.
  std::string confPath;
  std::string positionalRomPath;
  bool noState = false;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--conf") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "pc1500emu: --conf requires a path argument\n");
        return 1;
      }
      confPath = argv[++i];
    } else if (arg == "--no-state") {
      noState = true;
    } else if (positionalRomPath.empty()) {
      positionalRomPath = arg;
    }
  }

  // No --conf given: look for a default conf file (pc1500emu.json) in the
  // current directory, then next to the executable, then in the user's
  // home directory -- so settings persist across launches with zero
  // command-line involvement, not just when --conf is spelled out.
  if (confPath.empty()) {
    auto found = pc1500host::findDefaultConfFile(argv[0]);
    if (found) confPath = *found;
  }

  pc1500host::AppConfig appConfig;
  if (!confPath.empty()) {
    std::string confErr;
    bool confExisted = std::filesystem::exists(confPath);
    if (!pc1500host::loadAppConfig(confPath, &appConfig, &confErr)) {
      std::fprintf(stderr, "pc1500emu: could not read conf file '%s': %s\n", confPath.c_str(),
                   confErr.c_str());
      return 1;
    }
    // Rewrite immediately so a conf file from an older build (missing a
    // setting added since, e.g. showStatusPanel) is upgraded to the
    // current schema on disk right away -- loadAppConfig already defaults
    // any missing field in memory (see its own comment), but without this
    // the file itself would only catch up whenever some other setting
    // next happens to change (persistActiveConf, below), which could be
    // never. Best-effort: a write failure here isn't fatal to startup, and
    // will surface the next time a setting actually changes.
    if (confExisted) {
      std::string saveErr;
      pc1500host::saveAppConfig(appConfig, confPath, &saveErr);
    }
  }

  std::string effectiveRomPath =
      !positionalRomPath.empty() ? positionalRomPath : appConfig.romPath.value_or(std::string());
  if (!effectiveRomPath.empty()) {
    std::vector<uint8_t> rom = readFile(effectiveRomPath.c_str());
    if (rom.empty()) {
      std::fprintf(stderr, "pc1500emu: could not read ROM file '%s'\n", effectiveRomPath.c_str());
      return 1;
    }
    bus.loadME0(0xC000, rom.data(), rom.size());
    std::printf("pc1500emu: loaded %zu-byte ROM from %s\n", rom.size(), effectiveRomPath.c_str());
  } else {
    std::printf(
        "pc1500emu: no ROM given (run '%s -h' for usage) -- running with ROM area "
        "0xFF-filled\n",
        argv[0]);
  }

  // State-file restore happens instead of cpu.reset() -- resuming a saved
  // session should land exactly where OFF left the machine, the same way
  // a real PC-1500's OFF/ON cycle just halts and wakes the CPU in place
  // rather than resetting it (see state_file.h's comment: an earlier
  // version of this called cpu.reset() after restoring, which made every
  // resume incorrectly show the ROM's "NEW0?:CHECK" cold-start prompt --
  // real hardware only ever shows that after an actual RESET/battery-
  // change/module-change, never a plain OFF/ON). A missing file at the
  // configured path isn't an error (first run, nothing saved yet) --
  // cpu.reset() still runs in that case, same as always. A present-but-
  // corrupt one is a non-fatal warning, also falling back to reset().
  // --no-state clears this entirely, not just skips the load below --
  // leaving it set would still show the configured path in the status
  // panel, and worse, autoSaveOnExit (checked much later, purely on
  // "!configuredStateFilePath.empty()") would silently overwrite that
  // saved file with this run's fresh, empty session on exit. Treating
  // --no-state as "no state file configured this session" avoids both.
  // The persisted conf file itself is untouched, so a normal launch
  // (without --no-state) still resumes as configured.
  // Applied unconditionally, *before* attempting any state-file load below
  // -- not just on a no-state-restored fallback path. Bus::loadState (see
  // its own comment) now refuses to restore a state whose saved config
  // doesn't match the Bus's current config at the time it's called, so
  // there has to be a well-defined "current config" (this) for it to
  // compare against regardless of whether a restore ends up happening.
  // Variant first -- the RAM fields below are interpreted relative to
  // whichever variant is current (Bus::extRamExtBase()).
  bus.setMachineVariant(appConfig.isPC1500A ? pc1500::Bus::MachineVariant::PC1500A
                                             : pc1500::Bus::MachineVariant::PC1500);
  bus.setExtRamExtSize(appConfig.extRamExtBytes);
  bus.setExtRam0000Size(appConfig.extRam0000Bytes);
  bus.setCe163Enabled(appConfig.ce163Enabled);
  // Last, so it deterministically wins if a hand-edited conf file somehow
  // has a contradictory combination -- setCe155Enabled(true) itself
  // clears the two sizes and CE-163 right back off (see its own comment).
  bus.setCe155Enabled(appConfig.ce155Enabled);

  std::string configuredStateFilePath =
      noState ? std::string() : appConfig.stateFilePath.value_or(std::string());
  bool stateRestored = false;
  // Populated below only when the load fails specifically because the
  // state file's saved RAM config doesn't match appConfig's current
  // settings -- surfaced later, once the main window/ImGui context exist,
  // as a startup popup offering to apply the saved config and retry
  // instead of silently discarding the session (see that popup's own
  // comment further down, near openStateMismatchPopup).
  bool pendingStateMismatch = false;
  pc1500::Bus::SavedConfig pendingStateMismatchConfig;
  std::string pendingStateMismatchPath;
  std::string pendingStateMismatchError;
  if (!configuredStateFilePath.empty() && appConfig.autoLoadOnStart) {
    std::ifstream probe(configuredStateFilePath, std::ios::binary);
    if (probe.good()) {
      probe.close();
      std::string stateErr;
      bool configMismatch = false;
      if (pc1500host::loadStateFile(cpu, bus, configuredStateFilePath, &stateErr, &configMismatch,
                                     &pendingStateMismatchConfig)) {
        std::printf("pc1500emu: restored state from %s\n", configuredStateFilePath.c_str());
        stateRestored = true;
      } else if (configMismatch) {
        pendingStateMismatch = true;
        pendingStateMismatchPath = configuredStateFilePath;
        pendingStateMismatchError = stateErr;
      } else {
        std::fprintf(stderr, "pc1500emu: could not load state file '%s': %s\n",
                     configuredStateFilePath.c_str(), stateErr.c_str());
      }
    }
  }
  if (!stateRestored) {
    // No state to restore (missing file, --no-state, autoLoadOnStart off,
    // or Bus::loadState rejected it -- config mismatch or corrupt/
    // truncated data) -- the config applied above is already correct
    // either way, so just cold-boot from it. If this was a config
    // mismatch, the startup popup below (triggered by pendingStateMismatch)
    // may still retry the load with the saved config applied -- that
    // retry's own cpu.loadState()/bus.loadState() calls fully overwrite
    // whatever this cold boot left behind, so running it unconditionally
    // here first is harmless.
    cpu.reset();
  }

  // Known limitation: HLT sets a one-way halted flag. Real hardware wakes
  // from HLT via interrupt, but this core doesn't implement interrupt
  // delivery yet (only the SIE/RIE/IE flag bookkeeping), so a HLT'd
  // program will not resume in this emulator.
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "pc1500emu: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  // Shown while a blocking operation (currently just Load BASIC Text's
  // multi-pass typing of a long real-world listing, which can take
  // several seconds of wall-clock time) is in progress -- see
  // basicTextLoadPending's own comment below.
  SDL_Cursor* waitCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
  SDL_Cursor* defaultCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);

  // Buzzer output. Not fatal if unavailable -- run silently rather than
  // refusing to start over something as inessential as sound.
  SDL_AudioSpec desiredAudioSpec{};
  desiredAudioSpec.freq = kAudioSampleRate;
  desiredAudioSpec.format = AUDIO_S16SYS;
  desiredAudioSpec.channels = 1;
  desiredAudioSpec.samples = 1024;
  desiredAudioSpec.callback = nullptr;  // pushed via SDL_QueueAudio instead
  SDL_AudioDeviceID audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desiredAudioSpec, nullptr, 0);
  if (audioDevice == 0) {
    std::fprintf(stderr, "pc1500emu: SDL_OpenAudioDevice failed (running without sound): %s\n",
                 SDL_GetError());
  } else {
    SDL_PauseAudioDevice(audioDevice, 0);
  }
  if (TTF_Init() != 0) {
    std::fprintf(stderr, "pc1500emu: TTF_Init failed: %s\n", TTF_GetError());
    return 1;
  }
  TTF_Font* indicatorFont = nullptr;
  std::string indicatorFontLastError;
  for (const char* candidate : kIndicatorFontPathCandidates) {
    indicatorFont = TTF_OpenFontIndex(candidate, kIndicatorFontPtSize, kIndicatorFontFaceIndex);
    if (indicatorFont) break;
    indicatorFontLastError = TTF_GetError();
  }
  if (!indicatorFont) {
    std::fprintf(stderr, "pc1500emu: could not load an indicator font -- tried:\n");
    for (const char* candidate : kIndicatorFontPathCandidates) {
      std::fprintf(stderr, "  %s\n", candidate);
    }
    std::fprintf(stderr, "last error: %s\n", indicatorFontLastError.c_str());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow(
      "pc1500emu v" PC1500EMU_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowW,
      appConfig.showStatusPanel ? kWindowHWithPanel : kWindowHNoPanel, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  Uint32 mainWindowID = SDL_GetWindowID(window);
  // No-op outside macOS -- see mac_activate.h/.mm.
  macActivateApp();

  IMGUI_CHECKVERSION();
  ImGuiContext* mainImguiCtx = ImGui::CreateContext();
  ImGuiIO& imguiIo = ImGui::GetIO();
  imguiIo.IniFilename = nullptr;  // no persisted window-layout state -- menu bar only
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  // Pre-render every indicator label once; each frame just picks which
  // (if any) cached texture to blit per slot, based on the live 764EH/
  // 764FH bits -- matches how the dot matrix itself is drawn (fixed
  // "lit" color, no per-frame text shaping cost).
  SDL_Color indicatorColor{0x20, 0x28, 0x18, 255};  // same "lit" tone as the dot matrix
  std::vector<std::pair<const char*, SDL_Texture*>> indicatorTextures;
  for (const char* text : kAllIndicatorTexts) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(indicatorFont, text, indicatorColor);
    if (!surf) {
      std::fprintf(stderr, "pc1500emu: TTF_RenderUTF8_Blended('%s') failed: %s\n", text,
                   TTF_GetError());
      return 1;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    indicatorTextures.emplace_back(text, tex);
  }
  auto indicatorTexture = [&](const char* text) -> SDL_Texture* {
    for (auto& [t, tex] : indicatorTextures) {
      if (t == text) return tex;  // pointer equality is fine -- all callers pass the kXxx constants
    }
    return nullptr;
  };

  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  // See the scriptable-command-interface comment (near QueuedKeyAction) for
  // the overall design.
#if defined(_WIN32)
  if (!cmdPipeInit(kCommandPipeName)) {
    std::fprintf(stderr, "pc1500emu: could not create command pipe '%s' (error %lu)\n", kCommandPipeName,
                 GetLastError());
  }
#else
  // mkfifo() is a no-op (EEXIST) on repeated launches -- the FIFO just
  // persists on disk between runs, same file every time.
  mkfifo(kCommandFifoPath, 0666);
  int cmdFifoFd = open(kCommandFifoPath, O_RDONLY | O_NONBLOCK);
  if (cmdFifoFd < 0) {
    std::fprintf(stderr, "pc1500emu: could not open command FIFO '%s': %s\n", kCommandFifoPath,
                 strerror(errno));
  }
#endif
  std::string cmdBuf;

  bool running = true;
  int cyclesSinceTimerTick = 0;

  // Debugger execution control -- see the "setbreakpoints"/"continue"/
  // "pause"/"debugstep"/"debugstatus" FIFO commands below, and their
  // integration into the main per-frame loop further down (search for
  // debugControlActive there). Inert (debugControlActive stays false, the
  // per-frame loop behaves exactly as it always has) until the first
  // debug command arrives -- once a debugger attaches, the per-frame
  // loop's own free-running stepping is replaced by breakpoint-aware
  // single-instruction stepping instead, for the rest of this process's
  // lifetime.
  bool debugControlActive = false;
  bool debugRunning = false;
  std::set<uint16_t> breakpoints;
  std::string lastStopReason = "entry";

  // Shared by `status`/`debugstatus` -- the same register/flag dump,
  // without either command's own leading state line.
  auto formatRegisters = [&]() {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    out << "P=" << cpu.p() << " A=" << static_cast<int>(cpu.a()) << " X=" << cpu.x()
        << " Y=" << cpu.y() << " U=" << cpu.u() << " S=" << cpu.s() << "\n";
    out << std::dec;
    out << "halted=" << cpu.halted() << " bf=" << cpu.bf() << " disp=" << cpu.disp()
        << " pu=" << cpu.pu() << " pv=" << cpu.pv() << " ie=" << cpu.flags().ie
        << " timerCounter=" << cpu.timerCounter() << "\n";
    uint8_t ind1 = bus.readME0(0x764E), ind2 = bus.readME0(0x764F);
    out << "ind1(764E)=" << std::hex << static_cast<int>(ind1) << " ind2(764F)="
        << static_cast<int>(ind2) << std::dec << " [busy=" << ((ind1 & 0x01) != 0)
        << " shift=" << ((ind1 & 0x02) != 0) << " small=" << ((ind1 & 0x08) != 0)
        << " def=" << ((ind1 & 0x80) != 0) << " run=" << ((ind2 & 0x40) != 0)
        << " pro=" << ((ind2 & 0x20) != 0) << " reserve=" << ((ind2 & 0x10) != 0) << "]\n";
    return out.str();
  };
  // Single-instruction step, updating the timer the same way every other
  // debug/trace command in this file already does -- shared by the
  // `continue`/`debugstep` command handlers below and the main loop's
  // debug-mode stepping branch, so both advance cyclesSinceTimerTick
  // consistently.
  auto debugStepOne = [&]() {
    int c = cpu.step();
    int used = (c > 0) ? c : 1;
    cyclesSinceTimerTick += used;
    bus.advanceCycles(used);
    // See Upd1990ac::useManualClock()'s own comment: advancing the RTC's
    // manual clock per instruction (scaled by its own cycle cost), rather
    // than letting real-time-dependent register reads call the host clock
    // directly, is what keeps WAIT/BEEP's poll loop from missing TP
    // transitions during a burst of instructions that executes in a tiny
    // fraction of a millisecond of real time.
    bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
    while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
      cpu.tickTimer();
      cyclesSinceTimerTick -= kCyclesPerTimerTick;
    }
    return used;
  };

  // Tracks which action F10/F12 actually triggered on press, so release
  // matches it even if Shift/Ctrl changed state while the key was held
  // (otherwise a modifier change mid-hold could release the wrong
  // PC-1500 key and leave the other one stuck "pressed" forever).
  bool f10IsRocker = false;
  // Pending symbol-tap sequences (see QueuedKeyAction and the comment
  // above kTapFrames), processed one action at a time regardless of host
  // key state.
  std::deque<QueuedKeyAction> symbolActionQueue;
  int symbolActionFramesRemaining = 0;
  // Whether the SDL_TEXTINPUT event a KEYDOWN is about to generate (if
  // any) should be ignored -- see its computation and the SDL_TEXTINPUT
  // branch, both in the event loop below, for why.
  bool suppressNextTextInput = false;
  // When true, real host keyboard input (SDL_KEYDOWN/KEYUP/TEXTINPUT
  // destined for the emulated keyboard) is ignored entirely -- only the
  // FIFO/pipe command interface can drive the emulator. Toggled via the
  // Settings menu or the `automation on`/`automation off` FIFO command, so
  // a session driving pc1500emu via scripted commands can't be disrupted
  // by an accidental real keypress landing on the window.
  bool automationMode = false;

  // Command FIFO dispatcher -- see kCommandFifoPath's comment. Parses one
  // line at a time; `type`/`key` append to the same queue live keyboard
  // symbol/digit input uses (so scripted and real typing interleave
  // naturally instead of racing); `peek`/`dump`/`status`/`display` write
  // their result to kResponsePath immediately (no queueing needed -- these
  // don't touch the keyboard at all).
  auto writeResponse = [&](const std::string& text) {
    std::ofstream f(kResponsePath, std::ios::trunc);
    f << text;
  };
  auto processCommand = [&](const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (cmd == "type") {
      std::string text;
      std::getline(iss, text);
      size_t start = text.find_first_not_of(' ');
      if (start != std::string::npos) text = text.substr(start);
      // charToTapActions folds 'a'-'z' to the same physical key as 'A'-'Z'
      // by design (case is a persistent ROM-side SML mode, not a separate
      // keystroke -- see its own doc comment) and is deliberately SML-
      // unaware, so genuine lowercase output needs SmlAwareTyper's own
      // live-seeded toggle tracking (shared with every other text-sending
      // path -- see its own comment).
      pc1500::basic::SmlAwareTyper smlTyper(bus);
      for (char c : text) {
        if (!smlTyper.typeChar(c, &symbolActionQueue)) {
          std::fprintf(stderr, "pc1500emu: 'type' has no mapping for char '%c' -- use 'key NAME'\n", c);
        }
      }
    } else if (cmd == "key") {
      std::string name;
      iss >> name;
      // "shift+NAME" queues a genuine PC-1500 Shift-tap before NAME's tap,
      // same mechanism charToTapActions uses for symbols -- for testing
      // host-Shift-combo behavior directly via the command interface.
      bool withShift = false;
      constexpr const char* kShiftPrefix = "shift+";
      if (name.rfind(kShiftPrefix, 0) == 0) {
        withShift = true;
        name = name.substr(std::strlen(kShiftPrefix));
      }
      pc1500::Key k;
      if (nameToKey(name, &k)) {
        if (withShift) {
          symbolActionQueue.push_back({pc1500::Key::Shift, true, kTapFrames});
          symbolActionQueue.push_back({pc1500::Key::Shift, false, kIdleFrames});
        }
        symbolActionQueue.push_back({k, true, kTapFrames});
        symbolActionQueue.push_back({k, false, kIdleFrames});
      } else {
        std::fprintf(stderr, "pc1500emu: 'key' has no mapping for name '%s'\n", name.c_str());
      }
    } else if (cmd == "peek") {
      long addr = 0;
      iss >> std::hex >> addr;
      writeResponse(std::to_string(bus.readME0(static_cast<uint16_t>(addr))));
    } else if (cmd == "poke") {
      long addr = 0, val = 0;
      iss >> std::hex >> addr >> val;
      bus.writeME0(static_cast<uint16_t>(addr), static_cast<uint8_t>(val));
    } else if (cmd == "automation") {
      // `automation on`/`automation off` (also accepts 1/0) -- see
      // automationMode's own comment above its declaration.
      std::string arg;
      iss >> arg;
      automationMode = (arg == "on" || arg == "1");
      writeResponse(automationMode ? "ON" : "OFF");
    } else if (cmd == "break") {
      // Scriptable equivalent of a plain (unmodified) F12 press+release --
      // see F12's own handler for why both pieces (setOnKeyLine's IF-bit
      // latch and requestMI) are needed together.
      // Optional trailing cycle count: if present, traces that many
      // cycles synchronously within this same command (like calltrace),
      // atomically with the interrupt request -- necessary to actually
      // observe the dispatch, since the live ~60fps loop keeps stepping
      // the CPU in real time between separate pipe commands and would
      // otherwise race a separate `trace` call, handling and returning
      // from the interrupt before it ever arrives.
      cpu.pressOnKey();
      bus.ioPort().setOnKeyLine(true);
      cpu.requestMI();
      long cycles = 0;
      iss >> std::dec >> cycles;
      if (cycles > 0) {
        std::ostringstream out;
        long i = 0;
        while (i < cycles) {
          uint16_t pBefore = cpu.p();
          uint8_t op = bus.readME0(pBefore);
          int c = cpu.step();
          int used = (c > 0) ? c : 1;
          i += used;
          cyclesSinceTimerTick += used;
          bus.advanceCycles(used);
          bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
          while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
            cpu.tickTimer();
            cyclesSinceTimerTick -= kCyclesPerTimerTick;
          }
          out << std::hex << std::uppercase;
          out.width(4); out.fill('0'); out << pBefore << " ";
          out.width(2); out.fill('0'); out << static_cast<int>(op) << "\n";
        }
        writeResponse(out.str());
      }
      bus.setKeyState(pc1500::Key::Off, false);
      bus.ioPort().setOnKeyLine(false);
    } else if (cmd == "reset") {
      // Same as Ctrl+F12 -- re-runs CPU::reset() without touching RAM.
      // Needed after setextram: the ROM only detects installed extension
      // RAM size at reset/cold-start (see README's "adding RAM" note), not
      // on the fly.
      cpu.reset();
    } else if (cmd == "setmachine") {
      // FIFO equivalent of the Settings > Base Unit menu items. <variant>
      // is "1500" or "1500a". Needs `reset` after, same as setextram --
      // the ROM only detects the installed RAM shape at cold-start.
      // Affects how setextram's own "ext" window is interpreted (see
      // Bus::extRamExtBase()), so send this first if changing both.
      std::string variant;
      iss >> variant;
      if (variant == "1500") {
        bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500);
      } else if (variant == "1500a") {
        bus.setMachineVariant(pc1500::Bus::MachineVariant::PC1500A);
      } else {
        std::fprintf(stderr, "pc1500emu: setmachine variant must be 1500 or 1500a\n");
      }
    } else if (cmd == "setextram") {
      // FIFO equivalent of the Settings > Extension RAM menu items, for
      // headless setups that need more than the bare built-in RAM (e.g.
      // running a real expansion-module binary like BASWORD). <window> is
      // "ext" (the expansion window -- 4800H-based on a PC-1500,
      // 5800H-based on a PC-1500A, see Bus::extRamExtBase()) or "0000";
      // <bytes> is decimal.
      std::string window;
      long bytes = 0;
      iss >> window >> std::dec >> bytes;
      if (window == "ext") {
        bus.setExtRamExtSize(static_cast<size_t>(bytes));
      } else if (window == "0000") {
        bus.setExtRam0000Size(static_cast<size_t>(bytes));
      } else {
        std::fprintf(stderr, "pc1500emu: setextram window must be ext or 0000\n");
      }
    } else if (cmd == "setce163") {
      // FIFO equivalent of the Settings > Extension RAM (0000H) > CE-163
      // menu item. <enabled> is 0 or 1. Mutually exclusive with both
      // setextram windows and CE-155 -- Bus::setCe163Enabled(true) clears
      // them all; any of them going active clears this back off (see
      // each's own comment in bus.h). Needs `reset` after, same as
      // setextram.
      long enabled = 0;
      iss >> std::dec >> enabled;
      bus.setCe163Enabled(enabled != 0);
    } else if (cmd == "setce155") {
      // FIFO equivalent of the Settings > Extension RAM (0000H) > CE-155
      // menu item. <enabled> is 0 or 1. Mutually exclusive with both
      // setextram windows, CE-163, and CE-168N, same as setce163 above.
      // Needs `reset` after.
      long enabled = 0;
      iss >> std::dec >> enabled;
      bus.setCe155Enabled(enabled != 0);
    } else if (cmd == "setce168n") {
      // FIFO equivalent of setce163, generalized: <banks> <firstRoBank>.
      // <banks> 0 disables the module (same "0 = off" convention as
      // setextram); banks at/above <firstRoBank> simulate flash (writes
      // silently discarded, but still loadable via `loadbinary`) -- see
      // Bus::setCe168nEnabled's own comment. Mutually exclusive with
      // setextram/setce163/setce155, same as those are with each other.
      // Needs `reset` after, same as setextram/setce163.
      long banks = 0, roBank = 0;
      iss >> std::dec >> banks >> roBank;
      bus.setCe168nEnabled(static_cast<uint8_t>(banks), static_cast<uint8_t>(roBank));
    } else if (cmd == "dump") {
      long start = 0, end = 0;
      iss >> std::hex >> start >> end;
      std::ostringstream out;
      for (long a = start; a <= end; a += 16) {
        out << std::hex << std::uppercase;
        out.width(4);
        out.fill('0');
        out << a << ": ";
        for (long b = a; b < a + 16 && b <= end; b++) {
          out.width(2);
          out.fill('0');
          out << static_cast<int>(bus.readME0(static_cast<uint16_t>(b))) << " ";
        }
        out << "\n";
      }
      writeResponse(out.str());
    } else if (cmd == "testcursorrepeat") {
      // Debug-only, temporary: reproduces the double-press bug scenario
      // atomically (press, advance, release, re-press, advance, report),
      // avoiding the real-time race a sequence of separate pipe commands
      // has against the live frame loop's own background stepping.
      // <cyclesFirstHold> <cyclesAfterRetap> decimal.
      long cyclesFirstHold = 0, cyclesAfterRetap = 0;
      iss >> std::dec >> cyclesFirstHold >> cyclesAfterRetap;
      auto advance = [&](long n) {
        long i = 0;
        while (i < n) {
          int c = cpu.step();
          int used = (c > 0) ? c : 1;
          i += used;
          cyclesSinceTimerTick += used;
          bus.advanceCycles(used);
          bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
          while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
            cpu.tickTimer();
            cyclesSinceTimerTick -= kCyclesPerTimerTick;
          }
        }
      };
      bus.setKeyState(pc1500::Key::Up, true);
      advance(cyclesFirstHold);
      long afterFirstHold = bus.debugCursorRepeatCycles();
      bool firedAfterFirstHold = bus.debugCursorRepeatFired();
      bus.setKeyState(pc1500::Key::Up, false);
      bus.setKeyState(pc1500::Key::Up, true);
      long afterRetap = bus.debugCursorRepeatCycles();
      advance(cyclesAfterRetap);
      long afterFinal = bus.debugCursorRepeatCycles();
      bool firedAfterFinal = bus.debugCursorRepeatFired();
      std::ostringstream out;
      out << "afterFirstHold=" << afterFirstHold << " firedAfterFirstHold=" << firedAfterFirstHold
          << " afterRetapImmediate=" << afterRetap << " afterFinal=" << afterFinal
          << " firedAfterFinal=" << firedAfterFinal;
      writeResponse(out.str());
    } else if (cmd == "status") {
      std::ostringstream out;
      out << formatRegisters();
      out << "cursorKeyHeld=" << bus.debugCursorKeyHeld()
          << " cursorRepeatCycles=" << bus.debugCursorRepeatCycles() << "\n";
      writeResponse(out.str());
    } else if (cmd == "setbreakpoints") {
      // Replaces the full breakpoint set (no args clears it) -- this is
      // what a debug adapter sends on every VS Code "set breakpoints for
      // this file" request, which always supplies the complete desired
      // set, not a delta.
      breakpoints.clear();
      long addr;
      while (iss >> std::hex >> addr) breakpoints.insert(static_cast<uint16_t>(addr));
      debugControlActive = true;
      writeResponse("OK");
    } else if (cmd == "continue") {
      // Step past the current PC unconditionally first -- otherwise
      // resuming from a breakpoint you're currently stopped at would
      // re-trigger it instantly and never make progress. The main
      // per-frame loop's debugControlActive branch (below) takes over
      // from here, checking breakpoints before each subsequent
      // instruction.
      debugControlActive = true;
      debugStepOne();
      debugRunning = true;
      lastStopReason.clear();
      writeResponse("OK");
    } else if (cmd == "pause") {
      debugControlActive = true;
      debugRunning = false;
      lastStopReason = "paused";
      writeResponse("OK");
    } else if (cmd == "debugstep") {
      debugControlActive = true;
      debugRunning = false;
      debugStepOne();
      lastStopReason = "step";
      writeResponse(formatRegisters());
    } else if (cmd == "debugstatus") {
      std::ostringstream out;
      out << (debugRunning ? "RUNNING" : "STOPPED") << " reason=" << lastStopReason << "\n";
      out << formatRegisters();
      writeResponse(out.str());
    } else if (cmd == "display") {
      std::ostringstream out;
      for (int row = 0; row < pc1500::Lcd::kRows; row++) {
        for (int col = 0; col < pc1500::Lcd::kColumns; col++) {
          out << (lcd.dot(col, row, cpu.disp(), readByte) ? '#' : '.');
        }
        out << "\n";
      }
      writeResponse(out.str());
    } else if (cmd == "displaytext") {
      // Reads the ROM's own 80-byte LCD text buffer (7BB0H-7BFFH, per the
      // PC-2 Assembly Language manual's "DISPLAY THROUGH A BUFFER" section --
      // system call E8CAH) directly as ASCII, terminated by the documented
      // 0DH end code -- ground truth for what's on screen (e.g. "ERROR 1",
      // "NEW0? :CHECK") without decoding LCD dot-matrix pixels against the
      // ROM's font, which is error-prone to eyeball and unnecessary since
      // the ROM already keeps the text itself in RAM before rendering it.
      static constexpr uint16_t kDisplayTextBufBase = 0x7BB0;
      static constexpr int kDisplayTextBufLen = 80;
      std::string text;
      for (int i = 0; i < kDisplayTextBufLen; i++) {
        uint8_t b = bus.readME0(static_cast<uint16_t>(kDisplayTextBufBase + i));
        if (b == 0x0D) break;
        text += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b)
                                         : '?';
      }
      writeResponse(text);
    } else if (cmd == "savebasic") {
      std::string path;
      iss >> path;
      std::string error;
      bool ok = saveBasicProgram(bus, path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "loadbasic") {
      std::string path;
      iss >> path;
      std::string error;
      bool ok = loadBasicProgram(bus, path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "savebasictext") {
      std::string path;
      iss >> path;
      std::string error;
      bool ok = saveBasicTextFile(bus, path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "typeline") {
      std::string line;
      std::getline(iss, line);
      size_t start = line.find_first_not_of(' ');
      if (start != std::string::npos) line = line.substr(start);
      std::string error;
      bool ok = typeImmediateLine(bus, cpu, line, &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "typelinetrace") {
      long cycles = 0;
      iss >> std::dec >> cycles;
      std::string line;
      std::getline(iss, line);
      size_t start = line.find_first_not_of(' ');
      if (start != std::string::npos) line = line.substr(start);
      std::string error, trace;
      bool ok = typeImmediateLineWithTrace(bus, cpu, line, cycles, &error, &trace);
      writeResponse(ok ? trace : ("ERROR: " + error));
    } else if (cmd == "typelinenoenter") {
      std::string line;
      std::getline(iss, line);
      size_t start = line.find_first_not_of(' ');
      if (start != std::string::npos) line = line.substr(start);
      std::string error;
      bool ok = typeImmediateLine(bus, cpu, line, &error, /*pressEnter=*/false);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "typelinepartialidle") {
      // <shortIdlePos decimal> <shortIdleFrames decimal> <text>
      long pos = 0, frames = 0;
      iss >> std::dec >> pos >> frames;
      std::string line;
      std::getline(iss, line);
      size_t start = line.find_first_not_of(' ');
      if (start != std::string::npos) line = line.substr(start);
      std::string error;
      bool ok = typeImmediateLinePartialIdle(bus, cpu, line, static_cast<int>(pos),
                                              static_cast<int>(frames), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "typelinepartialidletrace") {
      // <shortIdlePos decimal> <shortIdleFrames decimal> <tailChars decimal> <extraCycles decimal> <text>
      long pos = 0, frames = 0, tailChars = 0, extraCycles = 0;
      iss >> std::dec >> pos >> frames >> tailChars >> extraCycles;
      std::string line;
      std::getline(iss, line);
      size_t start = line.find_first_not_of(' ');
      if (start != std::string::npos) line = line.substr(start);
      std::string error, trace;
      bool ok = typeImmediateLinePartialIdleTraced(bus, cpu, line, static_cast<int>(pos),
                                                    static_cast<int>(frames),
                                                    static_cast<int>(tailChars), extraCycles, &error,
                                                    &trace);
      writeResponse(ok ? trace : ("ERROR: " + error));
    } else if (cmd == "typelinewatch") {
      // <watchAddr hex> <watchLen decimal> <extraCycles decimal> <text>
      long watchAddr = 0, watchLen = 0, extraCycles = 0;
      iss >> std::hex >> watchAddr >> std::dec >> watchLen >> extraCycles;
      std::string line;
      std::getline(iss, line);
      size_t start = line.find_first_not_of(' ');
      if (start != std::string::npos) line = line.substr(start);
      std::string error, watchLog;
      bool ok = typeImmediateLineWatch(bus, cpu, line, static_cast<uint16_t>(watchAddr),
                                        static_cast<int>(watchLen), extraCycles, &error, &watchLog);
      writeResponse(ok ? watchLog : ("ERROR: " + error));
    } else if (cmd == "loadbasictext") {
      std::string path;
      iss >> path;
      std::vector<uint8_t> fileData = readFile(path.c_str());
      std::string error;
      if (fileData.empty()) {
        writeResponse("ERROR: Could not read file (or file is empty).");
      } else {
        std::string text(fileData.begin(), fileData.end());
        bool ok = typeBasicProgramText(bus, cpu, text, kCyclesPerFrame, kCyclesPerTimerTick, &error);
        writeResponse(ok ? "OK" : ("ERROR: " + error));
      }
    } else if (cmd == "savebinary") {
      long addr = 0, len = 0;
      std::string path;
      iss >> std::hex >> addr >> len >> path;
      std::string error;
      bool ok = saveBinary(bus, static_cast<uint16_t>(addr), static_cast<uint32_t>(len),
                            path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "loadbinary") {
      long addr = 0;
      std::string path;
      iss >> std::hex >> addr >> path;
      std::string error;
      bool ok = loadBinary(bus, static_cast<uint16_t>(addr), path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "loadrommodule" || cmd == "loadrommodule2" || cmd == "loadrommodule3" ||
               cmd == "loadrommodule4") {
      // <base hex> <requirePv 0|1> <usePuBank 0|1> <path> -- "loadrommodule"
      // (no suffix) targets slot 1, matching the pre-four-slot command name;
      // loadrommodule2/3/4 target slots 2-4, all four independent (see
      // Bus::RomModule's own comment for why more than one can be live at
      // once).
      int slot = (cmd == "loadrommodule") ? 0 : (cmd.back() - '1');
      long base = 0, requirePv = 0, usePuBank = 0;
      std::string path;
      iss >> std::hex >> base >> std::dec >> requirePv >> usePuBank;
      iss >> path;
      std::string error;
      bool ok = loadRomModule(bus, slot, static_cast<uint16_t>(base), requirePv != 0,
                               usePuBank != 0, path.c_str(), &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "loadexpansionmodule" || cmd == "loadexpansionmodule2" ||
               cmd == "loadexpansionmodule3" || cmd == "loadexpansionmodule4") {
      // <base hex> <requirePv 0|1> <usePuBank 0|1> <dataWindowBase hex>
      // <dataWindowSize hex> <instructionAddr hex> <path> [sdDir] -- like
      // loadrommodule, but for a module with a genuinely writable data
      // window and mock command processing (see Bus::RomModule's own
      // comment and ExpansionMock). Same four independent slots as
      // loadrommodule, sharing the same slot array. Optional trailing
      // sdDir points the (single, Bus-wide) ExpansionMock's SD-card
      // commands at a real host directory -- see loadExpansionModule's
      // own comment; omitted leaves SD commands erroring ("no card").
      int slot = (cmd == "loadexpansionmodule") ? 0 : (cmd.back() - '1');
      long base = 0, requirePv = 0, usePuBank = 0, dataWindowBase = 0, dataWindowSize = 0,
           instructionAddr = 0;
      std::string path, sdDir;
      iss >> std::hex >> base >> std::dec >> requirePv >> usePuBank;
      iss >> std::hex >> dataWindowBase >> dataWindowSize >> instructionAddr;
      iss >> path;
      iss >> sdDir;
      std::string error;
      bool ok = loadExpansionModule(bus, slot, static_cast<uint16_t>(base), requirePv != 0,
                                     usePuBank != 0, static_cast<uint16_t>(dataWindowBase),
                                     static_cast<uint16_t>(dataWindowSize),
                                     static_cast<uint16_t>(instructionAddr), path.c_str(), sdDir,
                                     &error);
      writeResponse(ok ? "OK" : ("ERROR: " + error));
    } else if (cmd == "unloadrommodule" || cmd == "unloadrommodule2" ||
               cmd == "unloadrommodule3" || cmd == "unloadrommodule4") {
      int slot = (cmd == "unloadrommodule") ? 0 : (cmd.back() - '1');
      bus.unloadRomModule(slot);
    } else if (cmd == "call") {
      long addr = 0;
      iss >> std::hex >> addr;
      cpu.setP(static_cast<uint16_t>(addr));
    } else if (cmd == "calltrace") {
      // Debug-only: like `call` followed by `trace`, but atomic within one
      // command handler invocation (sets P, force-clears halted_, then
      // steps/records synchronously) -- unlike two separate pipe commands,
      // nothing races against this since the live ~60fps frame loop can't
      // run in between. Needed to trace from an arbitrary entry point
      // (e.g. mid-ROM, not the normal reset vector) without the free-running
      // loop advancing the CPU past the intended window before `trace`'s own
      // pipe message even arrives.
      long addr = 0, cycles = 0;
      iss >> std::hex >> addr >> std::dec >> cycles;
      cpu.setP(static_cast<uint16_t>(addr));
      cpu.setHalted(false);
      std::ostringstream out;
      long i = 0;
      while (i < cycles) {
        uint16_t pBefore = cpu.p();
        uint8_t op = bus.readME0(pBefore);
        int c = cpu.step();
        int used = (c > 0) ? c : 1;
        i += used;
        cyclesSinceTimerTick += used;
        bus.advanceCycles(used);
        bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
        while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          cpu.tickTimer();
          cyclesSinceTimerTick -= kCyclesPerTimerTick;
        }
        out << std::hex << std::uppercase;
        out.width(4); out.fill('0'); out << pBefore << " ";
        out.width(2); out.fill('0'); out << static_cast<int>(op) << " A=";
        out.width(2); out.fill('0'); out << static_cast<int>(cpu.a()) << " X=";
        out.width(4); out.fill('0'); out << cpu.x() << " Y=";
        out.width(4); out.fill('0'); out << cpu.y() << " U=";
        out.width(4); out.fill('0'); out << cpu.u() << " S=";
        out.width(4); out.fill('0'); out << cpu.s() << "\n";
      }
      writeResponse(out.str());
    } else if (cmd == "entertrace") {
      // Debug-only: atomically presses Enter, holds kTapFrames, releases,
      // waits kIdleFrames, then traces `cycles` more -- all as ONE
      // continuous, unbroken instruction stream, exactly like the proven
      // typeImmediateLineWithTrace() (see its own comment on why: an
      // earlier version that traced only *after* the settle window missed
      // fast-resolving dispatches entirely). Deliberately does NOT touch
      // `P`/halted_ the way `calltrace` does -- unlike jumping to a known
      // static address, a live keypress's dispatch is timing-sensitive, and
      // the ~60fps frame loop keeps running in the real-world gaps between
      // separate pipe commands; a `presskey`/`calltrace`/`releasekey`/
      // `calltrace` sequence sent as four separate commands lets the frame
      // loop make real progress in those gaps, which `calltrace`'s forced
      // `cpu.setP()` then silently discards. This command presses Enter and
      // traces in one shot instead, so nothing can race it.
      long cycles = 0;
      iss >> std::dec >> cycles;
      std::ostringstream out;
      auto traceStepCycles = [&](long n) {
        long i = 0;
        while (i < n) {
          uint16_t pBefore = cpu.p();
          uint8_t op = bus.readME0(pBefore);
          int c = cpu.step();
          int used = (c > 0) ? c : 1;
          i += used;
          cyclesSinceTimerTick += used;
          bus.advanceCycles(used);
          bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
          while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
            cpu.tickTimer();
            cyclesSinceTimerTick -= kCyclesPerTimerTick;
          }
          out << std::hex << std::uppercase;
          out.width(4); out.fill('0'); out << pBefore << " ";
          out.width(2); out.fill('0'); out << static_cast<int>(op) << " A=";
          out.width(2); out.fill('0'); out << static_cast<int>(cpu.a()) << " X=";
          out.width(4); out.fill('0'); out << cpu.x() << " Y=";
          out.width(4); out.fill('0'); out << cpu.y() << " U=";
          out.width(4); out.fill('0'); out << cpu.u() << " S=";
          out.width(4); out.fill('0'); out << cpu.s() << "\n";
        }
      };
      bus.setKeyState(pc1500::Key::Ent, true);
      traceStepCycles(static_cast<long>(kTapFrames) * kCyclesPerFrame);
      bus.setKeyState(pc1500::Key::Ent, false);
      traceStepCycles(static_cast<long>(kIdleFrames) * kCyclesPerFrame);
      traceStepCycles(cycles);
      writeResponse(out.str());
    } else if (cmd == "enterwatch") {
      // <watchAddr hex> <watchLen decimal> <cycles decimal> -- same atomic,
      // race-free Enter-press pattern as entertrace, but logs only writes
      // to a specific address (see typeImmediateLineWatch's own comment)
      // instead of every instruction. For watching what happens to some
      // piece of state during a keyword's own dispatch, not just during
      // typing of a later line.
      long watchAddr = 0, watchLen = 0, cycles = 0;
      iss >> std::hex >> watchAddr >> std::dec >> watchLen >> cycles;
      std::ostringstream out;
      std::vector<uint8_t> prevBytes(watchLen);
      for (long b = 0; b < watchLen; b++)
        prevBytes[b] = bus.readME0(static_cast<uint16_t>(watchAddr + b));
      auto watchStepCycles = [&](long n) {
        long i = 0;
        while (i < n) {
          uint16_t pBefore = cpu.p();
          int c = cpu.step();
          int used = (c > 0) ? c : 1;
          i += used;
          cyclesSinceTimerTick += used;
          bus.advanceCycles(used);
          bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
          while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
            cpu.tickTimer();
            cyclesSinceTimerTick -= kCyclesPerTimerTick;
          }
          bool changed = false;
          for (long b = 0; b < watchLen; b++) {
            if (bus.readME0(static_cast<uint16_t>(watchAddr + b)) != prevBytes[b]) changed = true;
          }
          if (changed) {
            out << std::hex << std::uppercase;
            out.width(4); out.fill('0'); out << pBefore << " wrote " << watchAddr << ": ";
            for (long b = 0; b < watchLen; b++) {
              out.width(2); out.fill('0'); out << static_cast<int>(prevBytes[b]) << " ";
            }
            out << "-> ";
            for (long b = 0; b < watchLen; b++) {
              uint8_t nv = bus.readME0(static_cast<uint16_t>(watchAddr + b));
              out.width(2); out.fill('0'); out << static_cast<int>(nv) << " ";
              prevBytes[b] = nv;
            }
            out << " A="; out.width(2); out.fill('0'); out << static_cast<int>(cpu.a());
            out << " X="; out.width(4); out.fill('0'); out << cpu.x();
            out << " Y="; out.width(4); out.fill('0'); out << cpu.y();
            out << " U="; out.width(4); out.fill('0'); out << cpu.u();
            out << " S="; out.width(4); out.fill('0'); out << cpu.s() << "\n";
          }
        }
      };
      bus.setKeyState(pc1500::Key::Ent, true);
      watchStepCycles(static_cast<long>(kTapFrames) * kCyclesPerFrame);
      bus.setKeyState(pc1500::Key::Ent, false);
      watchStepCycles(static_cast<long>(kIdleFrames) * kCyclesPerFrame);
      watchStepCycles(cycles);
      writeResponse(out.str());
    } else if (cmd == "presskey" || cmd == "releasekey") {
      // Direct, synchronous bus.setKeyState -- bypasses the symbolActionQueue
      // that type/key use (which only drains via the normal ~60fps frame
      // loop), so a `trace`/`run` right after this actually captures the
      // key's effect instead of racing an undrained queue.
      std::string name;
      iss >> name;
      pc1500::Key k;
      if (nameToKey(name, &k)) {
        bus.setKeyState(k, cmd == "presskey");
      } else {
        std::fprintf(stderr, "pc1500emu: no mapping for name '%s'\n", name.c_str());
      }
    } else if (cmd == "run") {
      long cycles = 0;
      iss >> std::dec >> cycles;
      long ticks = 0;
      for (long i = 0; i < cycles;) {
        int c = cpu.step();
        int used = (c > 0) ? c : 1;
        i += used;
        cyclesSinceTimerTick += used;
        bus.advanceCycles(used);
        bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
        while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          cpu.tickTimer();
          cyclesSinceTimerTick -= kCyclesPerTimerTick;
          ticks++;
        }
      }
    } else if (cmd == "trace") {
      long cycles = 0;
      iss >> std::dec >> cycles;
      std::ostringstream out;
      long i = 0;
      while (i < cycles) {
        uint16_t pBefore = cpu.p();
        uint8_t op = bus.readME0(pBefore);
        int c = cpu.step();
        int used = (c > 0) ? c : 1;
        i += used;
        cyclesSinceTimerTick += used;
        bus.advanceCycles(used);
        bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
        while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          cpu.tickTimer();
          cyclesSinceTimerTick -= kCyclesPerTimerTick;
        }
        out << std::hex << std::uppercase;
        out.width(4); out.fill('0'); out << pBefore << " ";
        out.width(2); out.fill('0'); out << static_cast<int>(op) << " A=";
        out.width(2); out.fill('0'); out << static_cast<int>(cpu.a()) << " X=";
        out.width(4); out.fill('0'); out << cpu.x() << " Y=";
        out.width(4); out.fill('0'); out << cpu.y() << " U=";
        out.width(4); out.fill('0'); out << cpu.u() << "\n";
      }
      writeResponse(out.str());
    } else if (!cmd.empty()) {
      std::fprintf(stderr, "pc1500emu: unknown command '%s'\n", cmd.c_str());
    }
  };

  // File menu dialog state. The dialog form gets its own separate OS
  // window (SDL_Window/SDL_Renderer/ImGuiContext), created on demand --
  // the main emulator window is too small to comfortably fit it (Paul's
  // report). Dear ImGui's SDL2 backend already self-filters events by the
  // window it was bound to at init (see ImGui_ImplSDL2_GetViewportForWindowID
  // in imgui_impl_sdl2.cpp), so routing every SDL event through *both*
  // contexts' ImGui_ImplSDL2_ProcessEvent (switching ImGui::SetCurrentContext
  // first) is safe -- whichever context wasn't bound to that event's window
  // just ignores it.
  enum class ActiveDialog {
    NoDialog,
    LoadBasic,
    SaveBasic,
    LoadBinary,
    SaveBinary,
    LoadRomModule,
    LoadBasicText,
    SaveBasicText,
    LoadState,
    SaveState
  };
  ActiveDialog activeDialog = ActiveDialog::NoDialog;
  char filenameBuf[512] = "";
  // BASIC-text dialogs' multi-line editor -- fixed-size like every other
  // dialog field here (e.g. filenameBuf above), sized generously above
  // any realistic PC-1500 program's text-listing length.
  static constexpr size_t kBasicTextBufSize = 32768;
  auto basicTextBuf = std::make_unique<char[]>(kBasicTextBufSize);
  basicTextBuf[0] = '\0';
  char addrBuf[8] = "4600";
  char lenBuf[8] = "100";
  char callAddrBuf[8] = "4000";
  bool callAddrTouched = false;  // stops callAddrBuf auto-tracking addrBuf once the user edits it directly
  bool autoCallOnLoad = false;
  // ROM module (CE-150/153/158) dialog fields -- see Bus::RomModule.
  // Defaults match CE-150 (the more common/simpler of the two presets).
  // romTargetSlot picks which of Bus's kNumRomModules slots the dialog
  // below actually loads into -- set right before startDialog(..
  // LoadRomModule..) by whichever "Load Module N..." menu item was
  // clicked.
  char romBaseBuf[8] = "A000";
  bool romRequirePv = false;
  bool romUsePuBank = false;
  int romTargetSlot = 0;
  // Bus never stores a loaded module's source file path (only the raw
  // bytes/base/requirePv/usePuBank) -- tracked here purely for the status
  // panel's display, set on a successful ActiveDialog::LoadRomModule.
  std::array<std::string, pc1500::Bus::kNumRomModules> loadedModulePaths;
  std::string dialogError;
  // One-shot trigger for the startup state-config-mismatch popup, below --
  // pendingStateMismatch itself (set at startup, before this scope even
  // exists) never clears, so this separate flag is what stops OpenPopup()
  // from being called again every single frame once the user has already
  // dismissed or acted on it once.
  bool stateMismatchPopupShown = false;
  // Session-wide state-file/conf-file settings. configuredStateFilePath is
  // set up earlier in main(), before the SDL/ImGui init (see the
  // command-line-parsing block above); activeConfPath starts at whatever
  // --conf gave (or findDefaultConfFile discovered) and is filled in
  // lazily by persistActiveConf the first time any state-related setting
  // actually needs saving, if it's still empty at that point.
  std::string activeConfPath = confPath;
  std::string stateActionStatus;
  int stateActionStatusFramesRemaining = 0;
  constexpr int kStateActionStatusFrames = kFramesPerSecond * 3;
  SDL_Window* dialogWindow = nullptr;
  SDL_Renderer* dialogRenderer = nullptr;
  ImGuiContext* dialogImguiCtx = nullptr;

  auto closeDialogWindow = [&]() {
    if (!dialogWindow) return;
    ImGui::SetCurrentContext(dialogImguiCtx);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(dialogImguiCtx);
    ImGui::SetCurrentContext(mainImguiCtx);
    SDL_DestroyRenderer(dialogRenderer);
    SDL_DestroyWindow(dialogWindow);
    dialogWindow = nullptr;
    dialogRenderer = nullptr;
    dialogImguiCtx = nullptr;
  };
  auto openDialogWindow = [&](const char* title, int width = 440, int height = 260) {
    closeDialogWindow();  // in case a different dialog was already open
    dialogWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width,
                                     height, SDL_WINDOW_SHOWN);
    dialogRenderer = SDL_CreateRenderer(dialogWindow, -1, SDL_RENDERER_ACCELERATED);
    dialogImguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(dialogImguiCtx);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL2_InitForSDLRenderer(dialogWindow, dialogRenderer);
    ImGui_ImplSDLRenderer2_Init(dialogRenderer);
    ImGui::SetCurrentContext(mainImguiCtx);
  };
  // Hoisted out of the File menu's own BeginMenu block (unlike the other
  // dialog helpers above, this used to be declared inside it) so the
  // keyboard-shortcut polling block below -- which runs every frame,
  // regardless of whether the File menu happens to be open -- can call it
  // too.
  auto startDialog = [&](ActiveDialog which, const char* title, int width = 440,
                          int height = 260, const char* prefill = nullptr) {
    activeDialog = which;
    if (prefill != nullptr) {
      std::strncpy(filenameBuf, prefill, sizeof(filenameBuf) - 1);
      filenameBuf[sizeof(filenameBuf) - 1] = '\0';
    } else {
      filenameBuf[0] = '\0';
    }
    dialogError.clear();
    callAddrTouched = false;
    std::strncpy(callAddrBuf, addrBuf, sizeof(callAddrBuf));
    openDialogWindow(title, width, height);
  };
  // requestPick opens a native OS file picker (so the user can browse
  // instead of typing a path from memory), filtered by patterns/description
  // but not restricted to it -- every OS file dialog lets the user pick
  // "All files" or type an unfiltered name regardless. onResult is called
  // with nullopt if the user cancelled, in which case it should do nothing
  // (matches cancelling the follow-up custom dialog).
  //
  // This app is single-threaded (CPU emulation, audio, and every window's
  // rendering all happen in one while(running) loop, below), and the native
  // picker can block for several seconds on this machine (see
  // nativeWinPickPathEarlyEvent's own comment) -- calling it directly would
  // freeze the whole app, not just whichever dialog asked for a path. So
  // the actual OS call always runs on a worker thread; onResult is called
  // once the main loop notices pendingPick's future is ready (see the poll
  // near the top of while(running)), not from the worker thread itself.
  // Only one pick can be in flight at a time -- a second request while
  // one's already pending is just dropped, matching "the button doesn't do
  // anything yet" rather than queuing or cancelling, since there's only one
  // native dialog to show at once anyway.
  //
  // title/description/defaultPath are taken by value (not const char*)
  // specifically so callers can pass a stack buffer built with snprintf
  // (e.g. the ROM Modules menu's per-slot dialog title) -- those buffers
  // go out of scope long before the worker thread actually runs, so a raw
  // pointer would dangle; copying into owned strings here, before
  // launching the thread, is what makes that safe. patterns stays
  // `const char*` since every call site only ever passes string literals
  // (static storage, safe to keep pointers to indefinitely).
  struct PendingPick {
    std::future<std::optional<std::string>> future;
    std::function<void(std::optional<std::string>)> onResult;
  };
  // Only ever touched from the main thread: requestPick sets it (called
  // from an ImGui button handler), and the poll near the top of
  // while(running) reads/clears it. Neither worker thread below ever
  // touches this directly.
  std::optional<PendingPick> pendingPick;
  auto requestPick = [&pendingPick](bool save, std::string title, std::vector<const char*> patterns,
                                     std::string description, std::string defaultPath,
                                     std::function<void(std::optional<std::string>)> onResult) {
    if (pendingPick) return;
#if defined(_WIN32)
    // See nativeWinPickPathEarlyEvent's own comment: the future here comes
    // from a promise fulfilled inside an IFileDialogEvents::OnFileOk
    // callback, not from the worker thread's own return -- the worker
    // thread keeps running (stuck inside Show()) well after this future is
    // already ready.
    auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
    std::future<std::optional<std::string>> future = promise->get_future();
    std::thread(nativeWinPickPathEarlyEvent, save, std::move(title), std::move(patterns),
                std::move(description), std::move(defaultPath), std::move(promise))
        .detach();
    pendingPick = PendingPick{std::move(future), std::move(onResult)};
#else
    pendingPick = PendingPick{
        std::async(std::launch::async,
                   [=]() -> std::optional<std::string> {
                     const char* result =
                         save ? tinyfd_saveFileDialog(title.c_str(), defaultPath.c_str(),
                                                       static_cast<int>(patterns.size()),
                                                       patterns.data(), description.c_str())
                              : tinyfd_openFileDialog(title.c_str(), defaultPath.c_str(),
                                                       static_cast<int>(patterns.size()),
                                                       patterns.data(), description.c_str(), 0);
                     if (result == nullptr) return std::nullopt;
                     return std::string(result);
                   }),
        std::move(onResult)};
#endif
  };
  // Shared by every Browse-style button (the BASIC Text dialog's, and the
  // "Read File" button further down) and by the Load action itself when a
  // filename was typed/pasted in without either of those being clicked:
  // reads filenameBuf into basicTextBuf. Declared here (once, in the scope
  // requestPick's async
  // callbacks capture from) rather than inline inside the per-frame dialog
  // render block it used to live in -- a callback that runs on a later
  // frame (once a pick resolves) can't safely reference a lambda that was
  // itself a per-frame local, since that local's stack storage is long
  // gone by the time the callback fires.
  auto loadFileIntoTextBuf = [&]() {
    std::vector<uint8_t> fileData = readFile(filenameBuf);
    if (fileData.empty()) {
      dialogError = "Could not read file (or file is empty).";
    } else {
      size_t n = std::min(fileData.size(), kBasicTextBufSize - 1);
      std::memcpy(basicTextBuf.get(), fileData.data(), n);
      basicTextBuf[n] = '\0';
    }
  };
  // Writes appConfig to activeConfPath, so any state-related setting
  // change (Load/Save State succeeding, either auto-load/auto-save toggle)
  // is remembered across launches -- not just when the user has explicitly
  // wired up a --conf file. If nothing's active yet (fresh run, no --conf
  // and findDefaultConfFile found nothing), this is the point a conf file
  // first comes into existence: defaults to kDefaultConfFileName in the
  // current directory, which is also the first place findDefaultConfFile
  // looks on a future launch, so it's picked up automatically from then on
  // with no further action needed.
  auto persistActiveConf = [&]() {
    bool isNew = activeConfPath.empty();
    if (isNew) {
      activeConfPath =
          (std::filesystem::current_path() / pc1500host::kDefaultConfFileName).string();
    }
    std::string err;
    if (!pc1500host::saveAppConfig(appConfig, activeConfPath, &err)) {
      stateActionStatus = "Could not save settings to " + activeConfPath + ": " + err;
    } else if (isNew) {
      stateActionStatus += " (settings will be remembered in " + activeConfPath + ")";
    }
    stateActionStatusFramesRemaining = kStateActionStatusFrames;
  };
  // Temporary: logs every real key event with a precise timestamp, so a
  // captured typing sample can be replayed against the C++ core exactly
  // as typed (see platformTempFilePath("pc1500emu_keycapture.log")).
  std::ofstream keyCaptureLog(platformTempFilePath("pc1500emu_keycapture.log"), std::ios::app);
  auto captureStart = std::chrono::steady_clock::now();

  // Audio sample budget is tracked against real elapsed wall-clock time,
  // not a fixed kAudioSamplesPerFrame assumption -- SDL_Delay is
  // imprecise and per-frame rendering cost varies, so a frame is never
  // exactly 1/kFramesPerSecond seconds of real time. Generating a fixed
  // sample count every frame regardless drifts out of sync with the
  // audio device's actual real-time consumption rate, which is exactly
  // what produced the crackling/stuttering Paul heard (2026-07-27): the
  // queue alternately starved and overflowed instead of tracking real
  // time. audioSampleAccumulator carries the fractional sample count
  // across frames so no time is lost to rounding.
  double audioSampleAccumulator = 0.0;
  auto lastAudioTime = std::chrono::steady_clock::now();

  // Tracks the buzzer signal's running DC level so it can be subtracted
  // out before playback (see kBuzzerDcBlockAlpha) -- confirmed necessary
  // by a real boot-noise investigation (2026-07-27): OPC's buzzer bit is a
  // real, disassembly-confirmed 0->1 write during ROM1.BIN's own boot-time
  // register-init table (E159-E167 writes OPC=0xC0), and it then stays at
  // a *static* 1 indefinitely (traced 20M+ cycles with no further OPC
  // writes while idling at the boot prompt). A real buzzer/speaker is
  // AC-coupled -- it only responds to the *changing* signal from actual
  // BEEP tone generation, not a level that's simply held constant -- but
  // this emulator's naive raw-level-to-signed-PCM mapping treated that one
  // static level change as a permanent DC offset, producing an audible
  // "click" at the transition and (on some audio backends) a sustained
  // hum from the DC offset itself, for a ROM write that should have been
  // inaudible. Initialized to -1.0 (buzzerOn()'s default-off level) rather
  // than 0.0 so there's no spurious transient before the ROM ever touches
  // OPC at all.
  double buzzerDcEstimate = -1.0;

  while (running) {
    // Deliver a requestPick() result once its worker thread finishes --
    // see requestPick's own comment. Checked once per frame, up front,
    // same as the command-pipe poll right below: whatever the callback
    // does (typically startDialog) should be visible in this frame's own
    // render pass, not delayed another frame.
    if (pendingPick && pendingPick->future.wait_for(std::chrono::seconds(0)) ==
                           std::future_status::ready) {
      std::optional<std::string> pickedPath = pendingPick->future.get();
      std::function<void(std::optional<std::string>)> onResult = std::move(pendingPick->onResult);
      pendingPick.reset();
      onResult(std::move(pickedPath));
    }

#if defined(_WIN32)
    cmdPipePoll(&cmdBuf);
#else
    if (cmdFifoFd >= 0) {
      char buf[4096];
      ssize_t n;
      while ((n = read(cmdFifoFd, buf, sizeof(buf))) > 0) {
        cmdBuf.append(buf, static_cast<size_t>(n));
      }
    }
#endif
    {
      size_t nl;
      while ((nl = cmdBuf.find('\n')) != std::string::npos) {
        std::string line = cmdBuf.substr(0, nl);
        cmdBuf.erase(0, nl + 1);
        processCommand(line);
      }
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui::SetCurrentContext(mainImguiCtx);
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (dialogImguiCtx) {
        ImGui::SetCurrentContext(dialogImguiCtx);
        ImGui_ImplSDL2_ProcessEvent(&event);
        ImGui::SetCurrentContext(mainImguiCtx);
      }
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (dialogWindow && event.type == SDL_WINDOWEVENT &&
                 event.window.windowID == SDL_GetWindowID(dialogWindow) &&
                 event.window.event == SDL_WINDOWEVENT_CLOSE) {
        activeDialog = ActiveDialog::NoDialog;
        closeDialogWindow();
      } else if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
                 event.key.windowID != mainWindowID) {
        // Belongs to the dialog window (or something else) -- its own
        // ImGui context already processed it above; never feed it to the
        // emulated keyboard regardless of focus/capture state.
      } else if (automationMode &&
                 (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
                  event.type == SDL_TEXTINPUT)) {
        // Automation mode: real host keyboard input never reaches the
        // emulated keyboard -- only presskey/releasekey/type/key over the
        // FIFO can. ImGui already saw this event above, so menu
        // interaction/dialogs still work normally.
      } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        bool pressed = (event.type == SDL_KEYDOWN);
        SDL_Keycode kc = event.key.keysym.sym;
        Uint16 mod = event.key.keysym.mod;
        bool shiftHeld = (mod & KMOD_SHIFT) != 0;
        bool ctrlHeld = (mod & KMOD_CTRL) != 0;
        {
          auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - captureStart)
                               .count();
          keyCaptureLog << elapsedUs << ',' << kc << ',' << (pressed ? 1 : 0) << ','
                        << (event.key.repeat ? 1 : 0) << ',' << mod << '\n';
          keyCaptureLog.flush();
        }
        // Whether the SDL_TEXTINPUT event this key's press is about to
        // generate (if any) should be ignored by the SDL_TEXTINPUT branch
        // below -- true when this keydown is already fully handled by a
        // keycode-based path (F10/F12/Insert/Delete/kKeyMap), or is an OS
        // auto-repeat (filtered here for the same anti-flood reason the
        // queued-tap paths below filter event.key.repeat). Recomputed on
        // every keydown, so it's always correct by the time the following
        // SDL_TEXTINPUT (if any) is processed -- SDL always delivers a
        // key's TEXTINPUT immediately after its KEYDOWN, never reordered.
        if (pressed) {
          suppressNextTextInput = event.key.repeat || kc == SDLK_F10 || kc == SDLK_F12 ||
                                   kc == SDLK_INSERT || kc == SDLK_DELETE || ctrlHeld;
          if (!suppressNextTextInput) {
            for (const KeyMapping& m : kKeyMap) {
              if (m.keycode == kc) {
                suppressNextTextInput = true;
                break;
              }
            }
          }
        }
        if (ctrlHeld && kc != SDLK_F12) {
          // Reserved for host-level menu shortcuts (see the
          // ImGui::Shortcut(...) polling block near BeginMainMenuBar) --
          // never forward a Ctrl-held combo to the emulated keyboard. F12
          // is excluded: Ctrl+F12 is the existing raw-SDL reset hotkey and
          // already handles ctrlHeld itself, immediately below.
        } else if (kc == SDLK_F12) {
          if (pressed && ctrlHeld) {
            cpu.reset();
          } else if (pressed && shiftHeld) {
            bus.setKeyState(pc1500::Key::Off, true);
          } else if (pressed) {
            cpu.pressOnKey();
            bus.ioPort().setOnKeyLine(true);
            // The ON key doubles as BREAK while a program is running.
            // setOnKeyLine(true) above latches the IF register bit real
            // hardware sets for this (see its own comment); requesting MI
            // here is what actually gets the CPU to notice it -- neither
            // alone is sufficient (confirmed by testing each in
            // isolation). Traced by finding what actually reaches the
            // literal "BREAK IN" string in ROM1.BIN's own message table.
            cpu.requestMI();
          } else {
            // Unconditionally release both -- whichever was actually
            // pressed gets released; releasing the other is a no-op.
            bus.setKeyState(pc1500::Key::Off, false);
            bus.ioPort().setOnKeyLine(false);
          }
        } else if (kc == SDLK_F10) {
          if (pressed) f10IsRocker = shiftHeld;
          bus.setKeyState(f10IsRocker ? pc1500::Key::UpDownRocker : pc1500::Key::Sml, pressed);
        } else if (kc == SDLK_INSERT || kc == SDLK_DELETE) {
          // Both keys have exactly one PC-1500 meaning regardless of host
          // Shift (there's no plain-key gesture for either), and neither
          // produces SDL_TEXTINPUT (not printable characters) -- so unlike
          // the printable punctuation handled by SDL_TEXTINPUT below, these
          // stay a small dedicated keycode dispatch.
          if (pressed && !event.key.repeat) {
            pc1500::Key target = (kc == SDLK_INSERT) ? pc1500::Key::Right : pc1500::Key::Left;
            symbolActionQueue.push_back({pc1500::Key::Shift, true, kTapFrames});
            symbolActionQueue.push_back({pc1500::Key::Shift, false, kIdleFrames});
            symbolActionQueue.push_back({target, true, kTapFrames});
            symbolActionQueue.push_back({target, false, kIdleFrames});
          }
          // Release: nothing to do -- the queue above governs the target
          // key's own release timing independently of the host key.
        } else {
          for (const KeyMapping& m : kKeyMap) {
            if (m.keycode == kc) {
              if (isImmediateHostKey(m.key)) {
                // Ignore OS-level key-repeat here too (repeat is only ever
                // true for a KEYDOWN, never a KEYUP, so this only filters
                // the press side) -- without this, a held key's repeated
                // synthetic KEYDOWNs re-called setKeyState(key,true) on
                // top of an already-pressed key, which for cursor keys
                // fed spurious presses into Bus's own repeat-timing state
                // (see setKeyState's cursor-key comment) and could double
                // up a single physical tap. Bus's own kCursorRepeatCycles
                // mechanism (advanceCycles) is the sole intended source of
                // auto-repeat for a genuinely held cursor key.
                if (!event.key.repeat) bus.setKeyState(m.key, pressed);
              } else if (pressed) {
                // Ordinary text key: queue a fire-and-forget tap
                // (see kKeyMap's comment above) instead of reflecting
                // host press/release directly -- guarantees the ROM's
                // own minimum settle gap between keys regardless of how
                // fast the host reports them. Ignore OS key-repeat
                // (a long host hold) so it doesn't flood the queue.
                if (!event.key.repeat) {
                  symbolActionQueue.push_back({m.key, true, kTapFrames});
                  symbolActionQueue.push_back({m.key, false, kIdleFrames});
                }
              }
              // Release of a queued key: nothing to do -- the queued
              // release above already governs timing independently of
              // when the host key physically comes up.
              break;
            }
          }
        }
      } else if (event.type == SDL_TEXTINPUT && event.text.windowID != mainWindowID) {
        // Belongs to the dialog window -- its own ImGui context already
        // processed it above; never feed it to the emulated keyboard.
      } else if (event.type == SDL_TEXTINPUT) {
        // The layout-aware source for every printable/symbol key not
        // already handled directly above -- see the comment above
        // kTapFrames for why this replaced keycode-based symbol tables.
        // suppressNextTextInput (computed on the preceding KEYDOWN, above)
        // skips this whenever that key was already handled there, so nothing
        // ever fires twice.
        if (!suppressNextTextInput) {
          for (const char* p = event.text.text; *p != '\0'; ++p) {
            charToTapActions(*p, &symbolActionQueue);
          }
        }
      }
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Set inside the Help menu below, consumed just before BeginPopupModal
    // further down: OpenPopup()'s id is hashed against the *current* ImGui
    // window, which inside a MenuItem callback is the main menu bar's own
    // implicit window, not the outer window BeginPopupModal is called from
    // -- calling OpenPopup directly from the MenuItem hashes to a different
    // id than BeginPopupModal looks up, so the popup would never open.
    bool openAboutPopup = false;
    bool openSpecialKeysPopup = false;
    // One-shot: OpenPopup() is called from inside this per-frame block
    // (not from the startup code that actually set pendingStateMismatch,
    // which runs before any ImGui context/frame exists), gated on
    // stateMismatchPopupShown so it only fires once even though this
    // whole block re-runs every frame.
    bool openStateMismatchPopup = pendingStateMismatch && !stateMismatchPopupShown;
    if (openStateMismatchPopup) stateMismatchPopupShown = true;

    if (ImGui::BeginMainMenuBar()) {
      // Real keyboard shortcuts for the most-used menu actions -- polled
      // every frame regardless of which (if any) menu is currently open,
      // calling the exact same action each corresponding MenuItem does.
      // RouteGlobal (not RouteAlways) is deliberate: it's a registered
      // route ImGui resolves against other active routes each frame, so a
      // focused InputText's own built-in Ctrl+C/V/X/A/Z/Y handling still
      // wins -- no manual conflict-avoidance needed. The SDL keydown
      // handler above (see the `ctrlHeld && kc != SDLK_F12` guard) already
      // ensures none of these also leak through to the emulated keyboard.
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) {
        requestPick(false, "Load Binary", {"*.BIN", "*.bin"}, "Binary programs", "",
                    [&](std::optional<std::string> path) {
                      if (path) startDialog(ActiveDialog::LoadBinary, "Load Binary", 440, 260, path->c_str());
                    });
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        requestPick(true, "Save Binary", {"*.BIN", "*.bin"}, "Binary programs", "",
                    [&](std::optional<std::string> path) {
                      if (path) startDialog(ActiveDialog::SaveBinary, "Save Binary", 440, 260, path->c_str());
                    });
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) {
        requestPick(false, "Load BASIC", {"*.BAS", "*.bas"}, "BASIC programs", "",
                    [&](std::optional<std::string> path) {
                      if (path) startDialog(ActiveDialog::LoadBasic, "Load BASIC", 440, 260, path->c_str());
                    });
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        requestPick(true, "Save BASIC", {"*.BAS", "*.bas"}, "BASIC programs", "",
                    [&](std::optional<std::string> path) {
                      if (path) startDialog(ActiveDialog::SaveBasic, "Save BASIC", 440, 260, path->c_str());
                    });
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_L, ImGuiInputFlags_RouteGlobal)) {
        requestPick(false, "Load State", {"*.state"}, "State files", configuredStateFilePath,
                    [&](std::optional<std::string> path) {
                      if (path) startDialog(ActiveDialog::LoadState, "Load State", 440, 260, path->c_str());
                    });
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, ImGuiInputFlags_RouteGlobal)) {
        requestPick(true, "Save State", {"*.state"}, "State files", configuredStateFilePath,
                    [&](std::optional<std::string> path) {
                      if (path) startDialog(ActiveDialog::SaveState, "Save State", 440, 260, path->c_str());
                    });
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) {
        startDialog(ActiveDialog::LoadBasicText, "Load BASIC Text", 700, 500);
        basicTextBuf[0] = '\0';
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        startDialog(ActiveDialog::SaveBasicText, "Save BASIC Text", 700, 500);
        std::string text, saveErr;
        if (pc1500::basic::detokenizeBasicProgram(readBasicProgramBytes(bus, &saveErr), &text,
                                                    &saveErr)) {
          std::strncpy(basicTextBuf.get(), text.c_str(), kBasicTextBufSize - 1);
          basicTextBuf[kBasicTextBufSize - 1] = '\0';
        } else {
          basicTextBuf[0] = '\0';
          dialogError = saveErr;
        }
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_A, ImGuiInputFlags_RouteGlobal)) {
        automationMode = !automationMode;
      }
      if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_P, ImGuiInputFlags_RouteGlobal)) {
        appConfig.showStatusPanel = !appConfig.showStatusPanel;
        SDL_SetWindowSize(window, kWindowW,
                           appConfig.showStatusPanel ? kWindowHWithPanel : kWindowHNoPanel);
        persistActiveConf();
      }
      if (ImGui::BeginMenu("File")) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (ImGui::MenuItem("Load BASIC...", "Ctrl+Shift+O")) {
          requestPick(false, "Load BASIC", {"*.BAS", "*.bas"}, "BASIC programs", "",
                      [&](std::optional<std::string> path) {
                        if (path) startDialog(ActiveDialog::LoadBasic, "Load BASIC", 440, 260, path->c_str());
                      });
        }
        if (ImGui::MenuItem("Save BASIC...", "Ctrl+Shift+S")) {
          requestPick(true, "Save BASIC", {"*.BAS", "*.bas"}, "BASIC programs", "",
                      [&](std::optional<std::string> path) {
                        if (path) startDialog(ActiveDialog::SaveBasic, "Save BASIC", 440, 260, path->c_str());
                      });
        }
        ImGui::Separator();
        // Unlike the other File-menu items, these open the editor dialog
        // directly rather than a native picker first: the dialog's own
        // content (the text to load/save) needs review/editing regardless
        // of which file it goes to, so a native picker in front of it
        // would just be an extra click that saves nothing on its own --
        // exactly the confusion a native Save dialog's "Save" button
        // normally resolves by itself. Browse for a path from inside the
        // editor instead (its own "Browse..." button, next to Filename).
        if (ImGui::MenuItem("Load BASIC Text...", "Ctrl+Alt+O")) {
          startDialog(ActiveDialog::LoadBasicText, "Load BASIC Text", 700, 500);
          basicTextBuf[0] = '\0';
        }
        if (ImGui::MenuItem("Save BASIC Text...", "Ctrl+Alt+S")) {
          startDialog(ActiveDialog::SaveBasicText, "Save BASIC Text", 700, 500);
          std::string text, saveErr;
          if (pc1500::basic::detokenizeBasicProgram(readBasicProgramBytes(bus, &saveErr), &text,
                                                      &saveErr)) {
            std::strncpy(basicTextBuf.get(), text.c_str(), kBasicTextBufSize - 1);
            basicTextBuf[kBasicTextBufSize - 1] = '\0';
          } else {
            basicTextBuf[0] = '\0';
            dialogError = saveErr;
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Load Binary...", "Ctrl+O")) {
          requestPick(false, "Load Binary", {"*.BIN", "*.bin"}, "Binary programs", "",
                      [&](std::optional<std::string> path) {
                        if (path) startDialog(ActiveDialog::LoadBinary, "Load Binary", 440, 260, path->c_str());
                      });
        }
        if (ImGui::MenuItem("Save Binary...", "Ctrl+S")) {
          requestPick(true, "Save Binary", {"*.BIN", "*.bin"}, "Binary programs", "",
                      [&](std::optional<std::string> path) {
                        if (path) startDialog(ActiveDialog::SaveBinary, "Save Binary", 440, 260, path->c_str());
                      });
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("ROM Modules")) {
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
          // Four independent slots, not two -- see Bus::RomModule's own
          // comment: a real machine can have a module plugged in at both
          // the 0x8000 and 0xA000 bases at once, each with its own PV-low
          // and PV-high variant, so all four (base, PV) combinations can
          // be loaded and live simultaneously.
          for (int slot = 0; slot < pc1500::Bus::kNumRomModules; ++slot) {
            ImGui::PushID(slot);
            char menuLabel[24];
            std::snprintf(menuLabel, sizeof(menuLabel), "Load Module %d...", slot + 1);
            if (ImGui::MenuItem(menuLabel)) {
              char dialogTitleBuf[24];
              std::snprintf(dialogTitleBuf, sizeof(dialogTitleBuf), "Load ROM Module %d", slot + 1);
              // Copied to a std::string (not just passed as dialogTitleBuf) because
              // the callback below runs on a later frame, after this stack buffer
              // is gone -- requestPick's own title param is copied too, but that's
              // a separate copy only used for the picker itself, not this dialog.
              std::string dialogTitle(dialogTitleBuf);
              requestPick(false, dialogTitle, {"*.ROM", "*.rom", "*.BIN", "*.bin"}, "ROM images", "",
                          [&, slot, dialogTitle](std::optional<std::string> path) {
                            if (!path) return;
                            romTargetSlot = slot;
                            startDialog(ActiveDialog::LoadRomModule, dialogTitle.c_str(), 440, 260,
                                        path->c_str());
                            std::strncpy(romBaseBuf, "A000", sizeof(romBaseBuf));
                            romRequirePv = false;
                            romUsePuBank = false;
                          });
            }
            char ejectLabel[24];
            std::snprintf(ejectLabel, sizeof(ejectLabel), "Eject Module %d", slot + 1);
            if (ImGui::MenuItem(ejectLabel, nullptr, false, bus.romModuleLoaded(slot))) {
              bus.unloadRomModule(slot);
              loadedModulePaths[slot].clear();
            }
            ImGui::PopID();
          }
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Load State...", "Ctrl+L")) {
          requestPick(false, "Load State", {"*.state"}, "State files", configuredStateFilePath,
                      [&](std::optional<std::string> path) {
                        if (path) startDialog(ActiveDialog::LoadState, "Load State", 440, 260, path->c_str());
                      });
        }
        if (ImGui::MenuItem("Save State...", "Ctrl+Shift+L")) {
          requestPick(true, "Save State", {"*.state"}, "State files", configuredStateFilePath,
                      [&](std::optional<std::string> path) {
                        if (path) startDialog(ActiveDialog::SaveState, "Save State", 440, 260, path->c_str());
                      });
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Settings")) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (ImGui::BeginMenu("Base Unit")) {
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
          bool isA = bus.machineVariant() == pc1500::Bus::MachineVariant::PC1500A;
          // Persisted to the conf file (AppConfig::isPC1500A) so it
          // survives a restart -- only takes effect on the *next*
          // reset/cold-start (see that field's own comment). Changes how
          // every item in the two Extension RAM submenus below is
          // interpreted (Bus::extRamExtBase()/extRamExtWindowMaxSize()),
          // so set this first if also changing those in the same session.
          auto setVariant = [&](pc1500::Bus::MachineVariant v) {
            bus.setMachineVariant(v);
            appConfig.isPC1500A = (v == pc1500::Bus::MachineVariant::PC1500A);
            appConfig.extRamExtBytes = bus.extRamExtSize();  // may have been clamped
            persistActiveConf();
          };
          if (ImGui::MenuItem("PC-1500", nullptr, !isA)) {
            setVariant(pc1500::Bus::MachineVariant::PC1500);
          }
          if (ImGui::MenuItem("PC-1500A", nullptr, isA)) {
            setVariant(pc1500::Bus::MachineVariant::PC1500A);
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Extension RAM (expansion window)")) {
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
          size_t cur = bus.extRamExtSize();
          uint16_t base = bus.extRamExtBase();
          size_t maxSize = bus.extRamExtWindowMaxSize();
          bool ce163 = bus.ce163Enabled();
          bool ce155 = bus.ce155Enabled();
          // Persisted to the conf file (AppConfig::extRamExtBytes) so it
          // survives a restart -- it only takes effect on the *next*
          // reset/cold-start either way (see that field's own comment), so
          // there's no live-apply subtlety to worry about here, just
          // remembering the choice.
          auto setExtRamExt = [&](size_t bytes) {
            bus.setExtRamExtSize(bytes);
            appConfig.extRamExtBytes = bytes;
            // Mutually exclusive with CE-163/CE-155 (see
            // Bus::setExtRamExtSize's own comment). Unconditional, same
            // reasoning as Bus::setExtRam0000Size's own fix: "None"
            // (bytes == 0) is itself one of this submenu's mutually-
            // exclusive alternatives now that CE-155 lives here too, so
            // it must clear CE-163/CE-155 as well, not just nonzero sizes
            // -- a real PC-1500(A) has one expansion port, so "None"
            // here means nothing at all is plugged into it.
            appConfig.ce163Enabled = false;
            appConfig.ce155Enabled = false;
            persistActiveConf();
          };
          char label[80];
          auto item = [&](const char* text, size_t bytes) {
            if (bytes == 0) {
              std::snprintf(label, sizeof(label), "%s", text);
            } else {
              std::snprintf(label, sizeof(label), "%s (%04XH-%04XH)", text, base,
                             static_cast<unsigned>(base + bytes - 1));
            }
            // "None" needs the extra !ce163/!ce155 guard since both of
            // those also leave extRamExtSize_ at 0 -- without it, "None"
            // would show as checked even while CE-163/CE-155 is what's
            // actually active. The nonzero sizes don't need this: both
            // setters always leave extRamExtSize_ at exactly 0.
            bool selected = (bytes == 0) ? (cur == 0 && !ce163 && !ce155) : (cur == bytes);
            if (ImGui::MenuItem(label, nullptr, selected)) setExtRamExt(bytes);
          };
          // Real 1982 hardware options were 4K (CE-151) and 8K here, on a
          // PC-1500 -- the landing address shifts 1000H higher on a
          // PC-1500A (see Bus::extRamExtBase()), and the window's own
          // full physical span shrinks from 10K to 6K accordingly (see
          // Bus::extRamExtWindowMaxSize()). None of the "full window"
          // sizes were real period-correct modules, but are easy to
          // emulate and physically possible with modern RAM.
          item("None", 0);
          item("4K (CE-151)", 0x1000);
          if (maxSize == 0x1800) {
            item("6K (full window)", 0x1800);
          } else {
            item("6K", 0x1800);
            item("8K", 0x2000);
            item("10K (full window)", maxSize);
          }
          // CE-155: a real 1982-era 8K module, split across both windows --
          // 6K filling this window plus 2K isolated at exactly 3800H-3FFFH
          // in the 0000H window (see that submenu's own note when this is
          // active). Listed here, not in the 0000H submenu, since most of
          // its capacity (6K of 8K) lives in this window. See
          // Bus::setCe155Enabled's own comment.
          {
            char ce155Label[48];
            std::snprintf(ce155Label, sizeof(ce155Label), "CE-155 (8K: %04XH-6FFFH + 3800H)", base);
            if (ImGui::MenuItem(ce155Label, nullptr, ce155)) {
              bus.setCe155Enabled(true);
              appConfig.ce155Enabled = true;
              appConfig.extRam0000Bytes = 0;
              appConfig.extRamExtBytes = 0;
              appConfig.ce163Enabled = false;
              persistActiveConf();
            }
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Extension RAM (0000H)")) {
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
          size_t cur = bus.extRam0000Size();
          bool ce163 = bus.ce163Enabled();
          bool ce155 = bus.ce155Enabled();
          auto setExtRam0000 = [&](size_t bytes) {
            bus.setExtRam0000Size(bytes);
            appConfig.extRam0000Bytes = bytes;
            // Unconditional, matching Bus::setExtRam0000Size's own fix --
            // None/16K/CE-163 are mutually exclusive within this one
            // submenu (CE-155's own toggle lives in the expansion-window
            // submenu now, but selecting anything here still must clear
            // it -- its isolated 2K at 3800H-3FFFH is part of this same
            // window), so persisting "None" must clear both flags, or a
            // later launch's setCe163Enabled/setCe155Enabled call (which
            // runs after setExtRam0000Size in main()'s own pre-reset
            // ordering) would silently re-enable whichever one was left
            // set in the conf file.
            appConfig.ce163Enabled = false;
            appConfig.ce155Enabled = false;
            persistActiveConf();
          };
          // Not a real 1982-era option at all (nothing plugged in there
          // back then), but physically possible now.
          if (ImGui::MenuItem("None", nullptr, cur == 0 && !ce163 && !ce155)) setExtRam0000(0);
          if (ImGui::MenuItem("16K (full window)", nullptr,
                               cur == pc1500::Bus::kExtRam0000WindowSize && !ce163 && !ce155)) {
            setExtRam0000(pc1500::Bus::kExtRam0000WindowSize);
          }
          // CE-163: a real 1982-era module, 32K banked two 16K halves at a
          // time into this same window -- mutually exclusive with both the
          // choices above and with the expansion window, since its own
          // bank-select range physically overlaps that window and a real
          // PC-1500(A) has only one expansion port. See
          // Bus::setCe163Enabled's own comment.
          if (ImGui::MenuItem("CE-163 (32K, banked)", nullptr, ce163)) {
            bus.setCe163Enabled(true);
            appConfig.ce163Enabled = true;
            appConfig.extRam0000Bytes = 0;
            appConfig.extRamExtBytes = 0;
            appConfig.ce155Enabled = false;
            persistActiveConf();
          }
          // CE-155's own toggle lives in the Extension RAM (expansion
          // window) submenu instead -- most of its 8K (6K of it) lives
          // there. This window only hosts its remaining isolated 2K at
          // 3800H-3FFFH, so just note that here rather than duplicating
          // the toggle (which would need careful handling to keep both
          // copies' checked-state and click behavior identical).
          if (ce155) {
            ImGui::Separator();
            ImGui::BeginDisabled();
            ImGui::MenuItem("3800H-3FFFH in use by CE-155 (see expansion window)");
            ImGui::EndDisabled();
          }
          ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Automation Mode (ignore host keyboard)", "Ctrl+Alt+A", automationMode)) {
          automationMode = !automationMode;
        }
        if (ImGui::MenuItem("Auto-Load State on Start", nullptr, appConfig.autoLoadOnStart)) {
          appConfig.autoLoadOnStart = !appConfig.autoLoadOnStart;
          persistActiveConf();
        }
        if (ImGui::MenuItem("Auto-Save State on Exit", nullptr, appConfig.autoSaveOnExit)) {
          appConfig.autoSaveOnExit = !appConfig.autoSaveOnExit;
          // Enabling this with no state file configured yet (never done an
          // explicit Load/Save State, and none carried over from an
          // existing conf) would otherwise silently do nothing at exit --
          // the exit-time save is gated on configuredStateFilePath being
          // non-empty (see its own block, near the end of main()), with no
          // error either way since there's nothing to report as failed.
          // Default it here the same way persistActiveConf lazily defaults
          // the conf path itself, so the checkbox actually does something
          // out of the box.
          if (appConfig.autoSaveOnExit && configuredStateFilePath.empty()) {
            constexpr const char* kDefaultStateFileName = "pc1500emu.state";
            configuredStateFilePath =
                (std::filesystem::current_path() / kDefaultStateFileName).string();
            appConfig.stateFilePath = configuredStateFilePath;
            stateActionStatus = "Auto-save will use " + configuredStateFilePath;
            stateActionStatusFramesRemaining = kStateActionStatusFrames;
          }
          persistActiveConf();
        }
        if (ImGui::MenuItem("Show Status Panel", "Ctrl+Alt+P", appConfig.showStatusPanel)) {
          appConfig.showStatusPanel = !appConfig.showStatusPanel;
          SDL_SetWindowSize(window, kWindowW,
                             appConfig.showStatusPanel ? kWindowHWithPanel : kWindowHNoPanel);
          persistActiveConf();
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Help")) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (ImGui::MenuItem("Special Keys...")) {
          openSpecialKeysPopup = true;
        }
        if (ImGui::MenuItem("About pc1500emu...")) {
          openAboutPopup = true;
        }
        ImGui::EndMenu();
      }
      if (automationMode) {
        // Visible notice so it's obvious real typing won't reach the
        // emulator -- deliberately in the menu bar itself, not just the
        // menu checkbox, so it's seen without opening Settings.
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "  [AUTOMATION MODE -- host keyboard ignored]");
      }
      if (stateActionStatusFramesRemaining > 0) {
        --stateActionStatusFramesRemaining;
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "  %s", stateActionStatus.c_str());
      }
      ImGui::EndMainMenuBar();
    }

    // Status panel below the LCD -- ROM/module/CPU details. ImGui text
    // (not the indicator bar's cached-SDL_ttf-texture approach): this
    // content changes every frame, so pre-caching doesn't help, and
    // ImGui's own text rendering has no meaningful per-frame cost for a
    // few lines. Positioned to start right where the no-panel window ends
    // (kWindowHNoPanel), spanning kStatusPanelHeight down to
    // kWindowHWithPanel. Settings > Show Status Panel toggles
    // appConfig.showStatusPanel (persisted like every other conf-file
    // setting) *and* resizes the actual window between kWindowHNoPanel and
    // kWindowHWithPanel to match -- see that toggle's own
    // SDL_SetWindowSize call, below in the Settings menu.
    if (appConfig.showStatusPanel) {
      float panelY = static_cast<float>(kWindowHNoPanel);
      ImGui::SetNextWindowPos(ImVec2(0, panelY));
      ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kWindowW), static_cast<float>(kStatusPanelHeight)));
      ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoScrollbar;
      if (ImGui::Begin("##statuspanel", nullptr, panelFlags)) {
        if (ImGui::BeginTable("##statustable", 3, ImGuiTableFlags_SizingStretchSame)) {
          ImGui::TableNextColumn();
          ImGui::TextUnformatted("ROM / RAM");
          ImGui::Separator();
          ImGui::Text("ROM: %s", effectiveRomPath.empty() ? "(none)" : effectiveRomPath.c_str());
          for (int slot = 0; slot < pc1500::Bus::kNumRomModules; ++slot) {
            if (bus.romModuleLoaded(slot)) {
              const auto& m = bus.romModule(slot);
              const std::string& p = loadedModulePaths[slot];
              ImGui::Text("M%d: %s @0x%04X PV=%s%s", slot + 1, p.empty() ? "(loaded)" : p.c_str(),
                          m.base, m.requirePv ? "high" : "low", m.usePuBank ? " Bank" : "");
            } else {
              ImGui::Text("M%d: (empty)", slot + 1);
            }
          }
          ImGui::Text("Base unit: %s",
                      bus.machineVariant() == pc1500::Bus::MachineVariant::PC1500A ? "PC-1500A" : "PC-1500");
          if (bus.ce155Enabled()) {
            ImGui::Text("Ext RAM: CE-155 (3800H + %04XH window)", bus.extRamExtBase());
          } else if (bus.extRamExtSize() > 0) {
            ImGui::Text("Ext RAM %04XH: %uK", bus.extRamExtBase(),
                        static_cast<unsigned>(bus.extRamExtSize() / 1024));
          } else {
            ImGui::Text("Ext RAM %04XH: off", bus.extRamExtBase());
          }
          if (bus.ce163Enabled()) {
            ImGui::Text("Ext RAM 0000H: CE-163 (bank %u)", static_cast<unsigned>(bus.ce163Bank()));
          } else if (bus.extRam0000Size() > 0) {
            ImGui::Text("Ext RAM 0000H: %uK", static_cast<unsigned>(bus.extRam0000Size() / 1024));
          } else {
            ImGui::TextUnformatted("Ext RAM 0000H: off");
          }

          ImGui::TableNextColumn();
          ImGui::TextUnformatted("CPU");
          ImGui::Separator();
          ImGui::Text("PC=0x%04X", cpu.p());
          ImGui::Text("ME=%s", bus.lastAccessedSpace() == pc1500::Bus::MemorySpace::ME1 ? "ME1" : "ME0");
          ImGui::Text("halted=%d", cpu.halted() ? 1 : 0);
          {
            const lh5801::Flags& flags = cpu.flags();
            ImGui::Text("C=%d V=%d Z=%d H=%d IE=%d", flags.c ? 1 : 0, flags.v ? 1 : 0, flags.z ? 1 : 0,
                        flags.h ? 1 : 0, flags.ie ? 1 : 0);
          }

          ImGui::TableNextColumn();
          ImGui::TextUnformatted("pc1500emu");
          ImGui::Separator();
          ImGui::Text("v%s", PC1500EMU_VERSION);
          ImGui::Text("State: %s",
                      configuredStateFilePath.empty() ? "(none)" : configuredStateFilePath.c_str());

          ImGui::EndTable();
        }
      }
      ImGui::End();
    }

    // Special Keys popup -- same plain-ImGui-modal approach as About,
    // below. Reference is `kKeyMap[]`/`charToTapActions()`'s own comments
    // (and README.md's "Keyboard mapping" section, which this mirrors):
    // PC-1500 keys without an obvious host equivalent live on F7-F12.
    if (openSpecialKeysPopup) {
      ImGui::OpenPopup("Special Keys");
    }
    if (ImGui::BeginPopupModal("Special Keys", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
      if (ImGui::BeginTable("##specialkeys", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        auto row = [](const char* key, const char* fn) {
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(key);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(fn);
        };
        row("F7", "CL");
        row("F8", "MODE");
        row("F9", "DEF");
        row("F10", "SML");
        row("Shift+F10", "Up/down rocker key");
        row("F11", "RCL");
        row("F12", "ON (also BREAK while a program is running)");
        row("Shift+F12", "OFF");
        row("Ctrl+F12", "RESET (host-only; mimics the ALL RESET pinhole)");
        row("Tab", "SHIFT (toggles SHIFT and clears on next keypress)");
        ImGui::EndTable();
      }
      ImGui::Separator();
      ImGui::TextUnformatted("Letters, numpad digits, arrows, F1-F6, Enter, and Space map");
      ImGui::TextUnformatted("directly. Typing a digit or punctuation reproduces the exact");
      ImGui::TextUnformatted("PC-1500 keypress that types it on real hardware.");
      ImGui::Separator();
      if (ImGui::Button("OK", ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    // About popup -- a plain ImGui modal in the main context, unlike the
    // Load/Save dialogs' separate-SDL-window machinery, since there's no
    // file path input involved, just a few read-only lines. OpenPopup is
    // called here (not from the MenuItem above) so it shares this scope's
    // window context with BeginPopupModal -- see openAboutPopup's comment.
    if (openAboutPopup) {
      ImGui::OpenPopup("About pc1500emu");
    }
    if (ImGui::BeginPopupModal("About pc1500emu", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
      ImGui::Text("pc1500emu v%s", PC1500EMU_VERSION);
      ImGui::TextUnformatted("A from-scratch Sharp PC-1500 / LH5801 emulator.");
      ImGui::Separator();
      ImGui::TextUnformatted("Copyright (c) 2026 Paul Chambre.");
      ImGui::TextUnformatted("Licensed under the Apache License, Version 2.0.");
      ImGui::Separator();
      if (ImGui::Button("OK", ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    // Startup state-config-mismatch popup -- Bus::loadState refuses to
    // restore a saved session whose RAM configuration doesn't match
    // appConfig's current settings (see its own comment: raw RAM contents
    // are only meaningful relative to the memory-map shape they were
    // saved under). Rather than just cold-booting and reporting this to
    // stderr only (invisible in a normal GUI launch), offer to apply the
    // saved configuration and retry the restore -- the session is still
    // sitting right there in the state file, just not currently loadable.
    if (openStateMismatchPopup) {
      ImGui::OpenPopup("Saved Session Configuration Mismatch");
    }
    if (ImGui::BeginPopupModal("Saved Session Configuration Mismatch", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
      // Fixed-height scrolling region for the path/reason -- both are
      // unbounded-length strings (a long install path, a full sentence
      // explaining the mismatch), but this whole popup has to fit inside
      // pc1500emu's own small LCD-emulator-sized main window (no
      // multi-viewport support), so it can't just grow to fit them: the
      // Apply/Start Fresh buttons below need a *fixed* height budget above
      // them, not one that varies with how long the saved path happens to
      // be.
      ImGui::TextWrapped("Could not restore the saved session -- its configuration doesn't "
                          "match the current Settings:");
      ImGui::BeginChild("##mismatchdetail", ImVec2(420, 80), true);
      ImGui::TextWrapped("%s", pendingStateMismatchPath.c_str());
      ImGui::Spacing();
      ImGui::TextWrapped("%s", pendingStateMismatchError.c_str());
      ImGui::EndChild();
      const char* variantStr =
          pendingStateMismatchConfig.machineVariant == pc1500::Bus::MachineVariant::PC1500A
              ? "PC-1500A"
              : "PC-1500";
      if (pendingStateMismatchConfig.ce163Enabled) {
        ImGui::Text("Saved config -- %s, 0000H: CE-163 (32K banked)", variantStr);
      } else if (pendingStateMismatchConfig.ce155Enabled) {
        ImGui::Text("Saved config -- %s, 0000H: CE-155 (8K)", variantStr);
      } else {
        ImGui::Text("Saved config -- %s, 0000H: %zu bytes, expansion: %zu bytes", variantStr,
                     pendingStateMismatchConfig.extRam0000Size,
                     pendingStateMismatchConfig.extRamExtSize);
      }
      ImGui::Separator();
      if (ImGui::Button("Apply Saved Configuration and Restore Session", ImVec2(380, 0))) {
        // Same order main.cpp's own startup block applies appConfig in --
        // variant first (the two size setters below interpret their bytes
        // relative to whichever variant is current), ce155 last so it
        // deterministically wins if the saved combination is ever
        // ambiguous (it shouldn't be -- these setters enforce mutual
        // exclusion, so a saved config could only have reached this
        // combination through the same ordering in the first place).
        bus.setMachineVariant(pendingStateMismatchConfig.machineVariant);
        bus.setExtRamExtSize(pendingStateMismatchConfig.extRamExtSize);
        bus.setExtRam0000Size(pendingStateMismatchConfig.extRam0000Size);
        bus.setCe163Enabled(pendingStateMismatchConfig.ce163Enabled);
        bus.setCe155Enabled(pendingStateMismatchConfig.ce155Enabled);
        appConfig.isPC1500A =
            pendingStateMismatchConfig.machineVariant == pc1500::Bus::MachineVariant::PC1500A;
        appConfig.extRamExtBytes = pendingStateMismatchConfig.extRamExtSize;
        appConfig.extRam0000Bytes = pendingStateMismatchConfig.extRam0000Size;
        appConfig.ce163Enabled = pendingStateMismatchConfig.ce163Enabled;
        appConfig.ce155Enabled = pendingStateMismatchConfig.ce155Enabled;
        persistActiveConf();
        std::string retryErr;
        if (pc1500host::loadStateFile(cpu, bus, pendingStateMismatchPath, &retryErr)) {
          stateActionStatus = "Restored session from " + pendingStateMismatchPath;
        } else {
          // Shouldn't happen -- config now matches what was just applied
          // from the same file's own header -- but report it plainly
          // rather than silently failing a second time if it somehow does
          // (e.g. the file was truncated/corrupted between the two reads).
          stateActionStatus = "Still could not restore session: " + retryErr;
        }
        stateActionStatusFramesRemaining = kStateActionStatusFrames;
        pendingStateMismatch = false;
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::Button("Start Fresh with Current Settings", ImVec2(380, 0))) {
        pendingStateMismatch = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    ImGui::Render();

    // The dialog form renders in its own OS window/context (see
    // openDialogWindow's comment) -- a fully separate NewFrame/Render/
    // Present cycle, bound to dialogRenderer instead of the main window's
    // renderer.
    if (dialogWindow) {
      ImGui::SetCurrentContext(dialogImguiCtx);
      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();

      const char* actionLabel = "";
      bool isBinary = false;
      bool isLoad = false;
      bool isRomModule = false;
      bool isBasicText = false;
      switch (activeDialog) {
        case ActiveDialog::LoadBasic:
          actionLabel = "Load";
          isLoad = true;
          break;
        case ActiveDialog::SaveBasic:
          actionLabel = "Save";
          break;
        case ActiveDialog::LoadBinary:
          actionLabel = "Load";
          isBinary = true;
          isLoad = true;
          break;
        case ActiveDialog::SaveBinary:
          actionLabel = "Save";
          isBinary = true;
          break;
        case ActiveDialog::LoadRomModule:
          actionLabel = "Load";
          isRomModule = true;
          break;
        case ActiveDialog::LoadBasicText:
          actionLabel = "Load";
          isLoad = true;
          isBasicText = true;
          break;
        case ActiveDialog::SaveBasicText:
          actionLabel = "Save";
          isBasicText = true;
          break;
        case ActiveDialog::LoadState:
          actionLabel = "Load";
          isLoad = true;
          break;
        case ActiveDialog::SaveState:
          actionLabel = "Save";
          break;
        case ActiveDialog::NoDialog:
          break;
      }
      ImGuiIO& dialogIo = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(dialogIo.DisplaySize);
      bool closeRequested = false;
      bool wantsBasicTextLoad = false;
      if (ImGui::Begin("##dialog", nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
        ImGui::InputText("Filename", filenameBuf, sizeof(filenameBuf));
        if (isBasicText) {
          ImGui::SameLine();
          if (ImGui::Button("Browse...")) {
            const char* title = isLoad ? "Load BASIC Text" : "Save BASIC Text";
            requestPick(!isLoad, title, {"*.BAS", "*.bas"}, "BASIC programs", "",
                        [&, isLoad](std::optional<std::string> path) {
                          if (!path) return;
                          std::strncpy(filenameBuf, path->c_str(), sizeof(filenameBuf) - 1);
                          filenameBuf[sizeof(filenameBuf) - 1] = '\0';
                          if (isLoad) loadFileIntoTextBuf();
                        });
          }
        }
        if (isBinary) {
          bool addrChanged = ImGui::InputText("Address (hex)", addrBuf, sizeof(addrBuf),
                                               ImGuiInputTextFlags_CharsHexadecimal);
          if (addrChanged && !callAddrTouched) {
            std::strncpy(callAddrBuf, addrBuf, sizeof(callAddrBuf));
          }
          if (!isLoad) {
            ImGui::InputText("Length (hex)", lenBuf, sizeof(lenBuf),
                              ImGuiInputTextFlags_CharsHexadecimal);
          } else {
            ImGui::Checkbox("Call after load", &autoCallOnLoad);
            if (autoCallOnLoad) {
              // Defaults to the load address (kept in sync until the user
              // edits this field directly -- see callAddrTouched), but can
              // be set independently: e.g. load a relocatable blob at one
              // address and jump into an entry point partway through it.
              if (ImGui::InputText("Call address (hex)", callAddrBuf, sizeof(callAddrBuf),
                                    ImGuiInputTextFlags_CharsHexadecimal)) {
                callAddrTouched = true;
              }
            }
          }
        }
        if (isRomModule) {
          ImGui::InputText("Base address (hex)", romBaseBuf, sizeof(romBaseBuf),
                            ImGuiInputTextFlags_CharsHexadecimal);
          ImGui::Text("PV level required:");
          ImGui::SameLine();
          if (ImGui::RadioButton("Low", !romRequirePv)) romRequirePv = false;
          ImGui::SameLine();
          if (ImGui::RadioButton("High", romRequirePv)) romRequirePv = true;
          ImGui::Checkbox("Bank-switch via PU (file must be twice the visible window)",
                           &romUsePuBank);
          ImGui::Separator();
          ImGui::TextDisabled("Presets:");
          ImGui::SameLine();
          if (ImGui::Button("CE-150")) {
            std::strncpy(romBaseBuf, "A000", sizeof(romBaseBuf));
            romRequirePv = false;
            romUsePuBank = false;
          }
          ImGui::SameLine();
          if (ImGui::Button("CE-158")) {
            std::strncpy(romBaseBuf, "8000", sizeof(romBaseBuf));
            romRequirePv = true;
            romUsePuBank = true;
          }
        }
        if (isBasicText) {
          if (isLoad) {
            ImGui::SameLine();
            ImGui::BeginDisabled(filenameBuf[0] == '\0');
            if (ImGui::Button("Read File")) {
              loadFileIntoTextBuf();
            }
            ImGui::EndDisabled();
          } else {
            if (ImGui::Button("Copy to Clipboard")) {
              ImGui::SetClipboardText(basicTextBuf.get());
            }
          }
          ImGui::InputTextMultiline("##basicText", basicTextBuf.get(), kBasicTextBufSize,
                                     ImVec2(-1, -60));
        }
        if (!dialogError.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
          ImGui::TextWrapped("%s", dialogError.c_str());
          ImGui::PopStyleColor();
        }
        bool doAction = ImGui::Button(actionLabel);
        ImGui::SameLine();
        bool doCancel = ImGui::Button("Cancel");
        // Enter submits, Escape cancels -- every dialog except
        // LoadBasicText, where Enter has to stay a plain newline while
        // typing/editing the BASIC program text (the multiline InputText,
        // above) rather than immediately submitting mid-edit. Raw
        // IsKeyPressed (not the Shortcut()/routing API the menu-bar
        // shortcuts use) is deliberate: it fires regardless of which field
        // currently has focus, matching a standard OK/Cancel dialog.
        if (activeDialog != ActiveDialog::LoadBasicText &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
          doAction = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
          doCancel = true;
        }
        if (doAction) {
          bool ok = false;
          switch (activeDialog) {
            case ActiveDialog::LoadBinary: {
              uint16_t addr = static_cast<uint16_t>(strtol(addrBuf, nullptr, 16));
              ok = loadBinary(bus, addr, filenameBuf, &dialogError);
              if (ok && autoCallOnLoad) {
                uint16_t callAddr = static_cast<uint16_t>(strtol(callAddrBuf, nullptr, 16));
                cpu.setP(callAddr);
              }
              break;
            }
            case ActiveDialog::SaveBinary: {
              uint16_t addr = static_cast<uint16_t>(strtol(addrBuf, nullptr, 16));
              uint32_t len = static_cast<uint32_t>(strtoul(lenBuf, nullptr, 16));
              ok = saveBinary(bus, addr, len, filenameBuf, &dialogError);
              break;
            }
            case ActiveDialog::LoadBasic:
              ok = loadBasicProgram(bus, filenameBuf, &dialogError);
              break;
            case ActiveDialog::SaveBasic:
              ok = saveBasicProgram(bus, filenameBuf, &dialogError);
              break;
            case ActiveDialog::LoadRomModule: {
              uint16_t base = static_cast<uint16_t>(strtol(romBaseBuf, nullptr, 16));
              ok = loadRomModule(bus, romTargetSlot, base, romRequirePv, romUsePuBank, filenameBuf,
                                  &dialogError);
              if (ok) loadedModulePaths[romTargetSlot] = filenameBuf;
              break;
            }
            case ActiveDialog::LoadBasicText:
              // If the text box is still empty, the user typed/pasted a
              // path into Filename and went straight for Load without
              // clicking Read File first -- read it now so Load works the
              // way it looks like it should. Once the box has *something*
              // in it (from Read File, Browse, Paste, or hand-typing),
              // leave it alone: Load must submit whatever's on screen, not
              // silently clobber edits by re-reading the file.
              if (basicTextBuf[0] == '\0' && filenameBuf[0] != '\0') {
                loadFileIntoTextBuf();
              }
              // Deferred, not called here directly -- typeBasicProgramText's
              // onProgress callback (below, after this frame's own
              // End()/Render()/Present()) needs to run its own complete
              // NewFrame/Begin/End/Render/Present cycles to repaint
              // "Loading..." periodically during a long call, which can't
              // happen while *this* frame's own Begin("##dialog") is still
              // open (ImGui doesn't support nesting frame cycles).
              wantsBasicTextLoad = true;
              break;
            case ActiveDialog::SaveBasicText: {
              std::ofstream f(filenameBuf, std::ios::binary);
              if (!f) {
                dialogError = "Could not open file for writing.";
                break;
              }
              size_t len = std::strlen(basicTextBuf.get());
              f.write(basicTextBuf.get(), static_cast<std::streamsize>(len));
              ok = true;
              break;
            }
            case ActiveDialog::LoadState: {
              std::string err;
              ok = pc1500host::loadStateFile(cpu, bus, filenameBuf, &err);
              if (ok) {
                // No cpu.reset() -- restoring full CPU state (including P
                // and halted) resumes exactly where the saved session left
                // off, matching a real PC-1500's OFF/ON cycle (see
                // state_file.h's comment for why a reset here was wrong).
                configuredStateFilePath = filenameBuf;
                appConfig.stateFilePath = configuredStateFilePath;
                stateActionStatus = "State loaded.";
                persistActiveConf();
              } else {
                dialogError = err;
              }
              break;
            }
            case ActiveDialog::SaveState: {
              std::string err;
              ok = pc1500host::saveStateFile(cpu, bus, filenameBuf, &err);
              if (ok) {
                configuredStateFilePath = filenameBuf;
                appConfig.stateFilePath = configuredStateFilePath;
                stateActionStatus = "State saved.";
                persistActiveConf();
              } else {
                dialogError = err;
              }
              break;
            }
            case ActiveDialog::NoDialog:
              break;
          }
          if (ok) closeRequested = true;
        }
        if (doCancel) closeRequested = true;
      }
      ImGui::End();

      ImGui::Render();
      SDL_SetRenderDrawColor(dialogRenderer, 0x30, 0x30, 0x30, 255);
      SDL_RenderClear(dialogRenderer);
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), dialogRenderer);
      SDL_RenderPresent(dialogRenderer);
      ImGui::SetCurrentContext(mainImguiCtx);
      SDL_PumpEvents();

      // Only reached once this frame's own dialog render is fully closed
      // out (End/Render/Present above) -- typeBasicProgramText's
      // onProgress callback below runs its own complete NewFrame/Begin/
      // End/Render/Present cycles to repaint "Loading..." (next to the
      // now-disabled Load/Cancel buttons) periodically during what can be
      // a multi-second call for a real-world listing with many long
      // lines, which would otherwise leave the window looking frozen with
      // no feedback (and, on Windows, eventually trigger the OS's own
      // "not responding" ghost cursor).
      if (wantsBasicTextLoad) {
        auto onProgress = [&]() {
          ImGui::SetCurrentContext(dialogImguiCtx);
          ImGui_ImplSDLRenderer2_NewFrame();
          ImGui_ImplSDL2_NewFrame();
          ImGui::NewFrame();
          ImGuiIO& progressIo = ImGui::GetIO();
          ImGui::SetNextWindowPos(ImVec2(0, 0));
          ImGui::SetNextWindowSize(progressIo.DisplaySize);
          // Mirrors the normal LoadBasicText layout exactly (Filename+Browse
          // row, Read File row, the big text box, *then* the action row) --
          // everything disabled/read-only, since it's non-interactive while
          // loading -- so nothing visibly jumps position compared to the
          // dialog's usual look. An earlier version put the Load/Cancel/
          // "Loading..." row first, which pushed the text box down;
          // confirmed wrong, fixed here.
          if (ImGui::Begin("##dialog", nullptr,
                            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
            ImGui::BeginDisabled();
            ImGui::InputText("Filename", filenameBuf, sizeof(filenameBuf));
            ImGui::SameLine();
            ImGui::Button("Browse...");
            ImGui::SameLine();
            ImGui::Button("Read File");
            ImGui::InputTextMultiline("##basicText", basicTextBuf.get(), kBasicTextBufSize,
                                       ImVec2(-1, -60));
            ImGui::Button("Load");
            ImGui::SameLine();
            ImGui::Button("Cancel");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted("Loading...");
          }
          ImGui::End();
          ImGui::Render();
          SDL_SetRenderDrawColor(dialogRenderer, 0x30, 0x30, 0x30, 255);
          SDL_RenderClear(dialogRenderer);
          ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), dialogRenderer);
          SDL_RenderPresent(dialogRenderer);
          ImGui::SetCurrentContext(mainImguiCtx);
          SDL_PumpEvents();
          forceWindowRepaint(dialogWindow);
        };
        SDL_SetCursor(waitCursor);
        onProgress();  // paint "Loading..." immediately, before the first line is even typed
        std::string loadError;
        bool ok = typeBasicProgramText(bus, cpu, basicTextBuf.get(), kCyclesPerFrame,
                                        kCyclesPerTimerTick, &loadError, onProgress);
        dialogError = loadError;
        SDL_SetCursor(defaultCursor);
        if (ok) closeRequested = true;
      }

      if (closeRequested) {
        activeDialog = ActiveDialog::NoDialog;
        closeDialogWindow();
      }
    }

    // Process one step of the pending symbol-tap queue per frame,
    // independent of host key state (see QueuedKeyAction).
    if (!symbolActionQueue.empty() && --symbolActionFramesRemaining <= 0) {
      QueuedKeyAction action = symbolActionQueue.front();
      symbolActionQueue.pop_front();
      bus.setKeyState(action.key, action.pressed);
      symbolActionFramesRemaining = action.framesToWait;
    }

    // step() checks for pending interrupts even while halted (and clears
    // halted_ if one dispatches), so keep calling it either way rather
    // than skipping entirely -- that's the only way HLT can ever resume.
    // Deliberately no early-exit on cpu.halted(): breaking out here would
    // stop the timer from advancing too, so a halted CPU would only ever
    // see its timer interrupt move forward by one cycle per rendered
    // frame (~9 minutes of real time to accumulate one interrupt period,
    // instead of ~25ms) -- exactly the kind of thing that makes keyboard
    // input look almost entirely unresponsive.
    auto audioNow = std::chrono::steady_clock::now();
    double elapsedSeconds = std::chrono::duration<double>(audioNow - lastAudioTime).count();
    lastAudioTime = audioNow;
    // Re-anchors the RTC's manual clock to the real host clock's current
    // instant for this frame -- see Upd1990ac::useManualClock()'s own
    // comment. Instructions stepped below (both the debug and normal
    // paths) advance it smoothly from here by their own cycle cost,
    // rather than every register read calling the host clock directly
    // and seeing one frozen snapshot for the whole frame's burst.
    bus.ioPort().useManualRtcClock();
    bus.pollRtc();
    audioSampleAccumulator += elapsedSeconds * kAudioSampleRate;
    int samplesThisFrame = static_cast<int>(audioSampleAccumulator);
    if (samplesThisFrame > kMaxAudioSamplesPerFrame) samplesThisFrame = kMaxAudioSamplesPerFrame;
    audioSampleAccumulator -= samplesThisFrame;

    int cyclesRun = 0;
    int16_t audioSamples[kMaxAudioSamplesPerFrame];
    int audioSamplesEmitted = 0;
    if (debugControlActive) {
      // A debugger has attached (see the "setbreakpoints"/"continue"/etc.
      // FIFO commands above) -- replace the normal free-running stepping
      // below with breakpoint-aware single-instruction stepping, spread
      // across real frames (so the GUI keeps rendering and a `pause`
      // command takes effect within about one instruction, rather than
      // this frame blocking until a whole continue's worth of cycles is
      // done). No audio sampling in this mode -- a debug session doesn't
      // care about the buzzer, and it'd have to be reworked to cope with
      // stepping being interrupted mid-frame by a breakpoint anyway.
      while (debugRunning && cyclesRun < kCyclesPerFrame) {
        uint16_t pBefore = cpu.p();
        if (breakpoints.count(pBefore)) {
          debugRunning = false;
          std::ostringstream reason;
          reason << "breakpoint 0x" << std::hex << std::uppercase << pBefore;
          lastStopReason = reason.str();
          break;
        }
        cyclesRun += debugStepOne();
      }
    } else {
      while (cyclesRun < kCyclesPerFrame) {
        int c = cpu.step();
        int used = (c > 0) ? c : 1;
        cyclesRun += used;
        cyclesSinceTimerTick += used;
        bus.advanceCycles(used);
        // See the useManualRtcClock() call above this loop.
        bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
        while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          cpu.tickTimer();
          cyclesSinceTimerTick -= kCyclesPerTimerTick;
        }

        // Sample the buzzer bit cycle-accurately (not from an SDL audio
        // callback thread -- see the buzzer comment above) so this frame's
        // rendered CPU execution maps proportionally across however many
        // samples this frame actually gets (samplesThisFrame, decided from
        // real elapsed time above).
        int targetSamples =
            static_cast<int>((static_cast<int64_t>(cyclesRun) * samplesThisFrame) / kCyclesPerFrame);
        if (targetSamples > samplesThisFrame) targetSamples = samplesThisFrame;
        double rawLevel = bus.ioPort().buzzerOn() ? 1.0 : -1.0;
        while (audioSamplesEmitted < targetSamples) {
          // DC-blocking high-pass filter -- see buzzerDcEstimate's comment
          // above. Applied per emitted sample (not per CPU step) since the
          // filter's time constant is defined in real sample-rate terms.
          buzzerDcEstimate += (rawLevel - buzzerDcEstimate) * kBuzzerDcBlockAlpha;
          double filtered = rawLevel - buzzerDcEstimate;
          audioSamples[audioSamplesEmitted++] =
              static_cast<int16_t>(filtered * kBuzzerAmplitude);
        }
      }
    }
    if (audioDevice != 0) {
      SDL_QueueAudio(audioDevice, audioSamples, static_cast<Uint32>(audioSamplesEmitted * sizeof(int16_t)));
    }

    SDL_SetRenderDrawColor(renderer, 0xC8, 0xD8, 0xC0, 255);  // unlit LCD background
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 0x20, 0x28, 0x18, 255);  // lit dot
    for (int col = 0; col < pc1500::Lcd::kColumns; col++) {
      for (int row = 0; row < pc1500::Lcd::kRows; row++) {
        if (lcd.dot(col, row, cpu.disp(), readByte)) {
          SDL_Rect r{kMarginLeft + col * kScale,
                     kMenuBarHeight + kIndicatorBarHeight + kMarginTop + row * kScale, kScale - 1,
                     kScale - 1};
          SDL_RenderFillRect(renderer, &r);
        }
      }
    }

    // Fixed-segment status indicators (764EH/764FH) -- left to right:
    // BUSY, SHIFT, katakana, SMALL, DEG/RAD/GRAD, RUN, PRO, RESERVE, DEF,
    // then I/II/III as a tight cluster (see kIndicatorSlotCount's comment).
    uint8_t ind1 = bus.readME0(0x764E);
    uint8_t ind2 = bus.readME0(0x764F);
    bool de = ind2 & 0x01, g = ind2 & 0x02, rad = ind2 & 0x04;
    const char* angleMode = nullptr;
    if (de && g) angleMode = kDeg;
    else if (g) angleMode = kGrad;
    else if (rad) angleMode = kRad;
    const char* slotText[kIndicatorSlotCount] = {
        (ind1 & 0x01) ? kBusy : nullptr,      // Busy
        (ind1 & 0x02) ? kShift : nullptr,     // Shift
        (ind1 & 0x04) ? kKatakana : nullptr,  // Japanese
        (ind1 & 0x08) ? kSmall : nullptr,     // Small
        angleMode,                            // Deg/Rad/Grad
        (ind2 & 0x40) ? kRun : nullptr,        // Run
        (ind2 & 0x20) ? kPro : nullptr,        // Pro
        (ind2 & 0x10) ? kReserve : nullptr,    // Reserve
        (ind1 & 0x80) ? kDef : nullptr,        // Def
    };
    int slotWidth = kWindowW / (kIndicatorSlotCount + 1);  // +1 reserves the last slot for I/II/III
    for (int slot = 0; slot < kIndicatorSlotCount; slot++) {
      if (!slotText[slot]) continue;
      SDL_Texture* tex = indicatorTexture(slotText[slot]);
      if (!tex) continue;
      int texW, texH;
      SDL_QueryTexture(tex, nullptr, nullptr, &texW, &texH);
      SDL_Rect dst{slot * slotWidth + (slotWidth - texW) / 2,
                   kMenuBarHeight + (kIndicatorBarHeight - texH) / 2, texW, texH};
      SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    // I/II/III: tight cluster, one space apart, centered in the last slot.
    const char* romanTexts[3] = {(ind1 & 0x40) ? kOne : nullptr, (ind1 & 0x20) ? kTwo : nullptr,
                                  (ind1 & 0x10) ? kThree : nullptr};
    int spaceW = 0, spaceH = 0;
    TTF_SizeUTF8(indicatorFont, " ", &spaceW, &spaceH);
    int romanTotalW = 0;
    for (const char* t : romanTexts) {
      if (!t) continue;
      int w, h;
      SDL_QueryTexture(indicatorTexture(t), nullptr, nullptr, &w, &h);
      romanTotalW += (romanTotalW > 0 ? spaceW : 0) + w;
    }
    int romanX = kIndicatorSlotCount * slotWidth + (slotWidth - romanTotalW) / 2;
    for (const char* t : romanTexts) {
      if (!t) continue;
      SDL_Texture* tex = indicatorTexture(t);
      int texW, texH;
      SDL_QueryTexture(tex, nullptr, nullptr, &texW, &texH);
      SDL_Rect dst{romanX, kMenuBarHeight + (kIndicatorBarHeight - texH) / 2, texW, texH};
      SDL_RenderCopy(renderer, tex, nullptr, &dst);
      romanX += texW + spaceW;
    }

    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

    SDL_RenderPresent(renderer);

    SDL_Delay(1000 / kFramesPerSecond);
  }

  if (appConfig.autoSaveOnExit && !configuredStateFilePath.empty()) {
    // Real hardware refuses OFF while a program is running -- BREAK first
    // is mandatory, so any state a real user could actually power off
    // from is always a break'd/idle one, never mid-run. Match that here:
    // send BREAK (same press/step/release sequence as the interactive
    // F12 key and the "break" FIFO command) before saving, so a resumed
    // session always lands somewhere the ROM's own state machine expects,
    // not frozen at an arbitrary mid-instruction point.
    //
    // BREAK alone isn't quite enough, though: even with nothing running,
    // it leaves the ROM's own "BREAK IN <line>" message on screen (real
    // hardware's ON/BREAK key always prints this on a genuine break,
    // whether or not a program actually was running when it fired), which
    // would otherwise resume looking like a program just got interrupted.
    // Sending CL afterward is how a real user would clear that back to a
    // plain command-line prompt before powering off -- so BREAK, then CL,
    // then save.
    auto stepCyclesForShutdown = [&](long cycles) {
      while (cycles > 0) {
        int used = cpu.step();
        if (used <= 0) used = 1;
        cycles -= used;
        cyclesSinceTimerTick += used;
        bus.advanceCycles(used);
        bus.ioPort().advanceManualRtcClock(static_cast<double>(used) / kCyclesPerSecond);
        while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
          cpu.tickTimer();
          cyclesSinceTimerTick -= kCyclesPerTimerTick;
        }
      }
    };

    cpu.pressOnKey();
    bus.ioPort().setOnKeyLine(true);
    cpu.requestMI();
    stepCyclesForShutdown(kCyclesPerSecond);  // generous, bounded budget
    bus.setKeyState(pc1500::Key::Off, false);
    bus.ioPort().setOnKeyLine(false);

    bus.setKeyState(pc1500::Key::Cl, true);
    stepCyclesForShutdown(kCyclesPerSecond);
    bus.setKeyState(pc1500::Key::Cl, false);

    std::string err;
    if (!pc1500host::saveStateFile(cpu, bus, configuredStateFilePath, &err)) {
      std::fprintf(stderr, "pc1500emu: auto-save on exit failed: %s\n", err.c_str());
    }
  }

  closeDialogWindow();
#if defined(_WIN32)
  cmdPipeClose();
#else
  if (cmdFifoFd >= 0) close(cmdFifoFd);
#endif

  for (auto& [text, tex] : indicatorTextures) {
    (void)text;
    SDL_DestroyTexture(tex);
  }
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  TTF_CloseFont(indicatorFont);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  TTF_Quit();
  if (audioDevice != 0) SDL_CloseAudioDevice(audioDevice);
  SDL_FreeCursor(waitCursor);
  SDL_FreeCursor(defaultCursor);
  SDL_Quit();
  return 0;
}
