// Game observer probes (M2 integration; see game_probes.h for the list).
//
// The M2 exit criteria (design 6, M2) need facts that only the game code knows: which .img archives it
// mounted, the video standard CApp+0xE8, the language it selected, the BUILD.TXT it read, whether the
// intro context runs, and that the disc error handler is never called. These probes log them on level
// info (GAME/VFS/XAPI) and feed vfs_trace.log with one line per archive lookup, so that the trace lists
// every asset the game opens - archive members as well as loose files (criterion 7, [FIL] 1.2).
//
// All probes are observers (FUT_PROBE: log, call the original through its trampoline, return its eax);
// `[hooks] disable = probe:Game::*` etc. switches them off. Prologues and returns were checked with
// capstone on orig/retail_eu/default.xbe (whole, position-independent instructions; no direct branch of
// a Ghidra function and no raw E8/E9 in 0x11000..0x293560 targets the inside of a stolen range; scratch
// script of the M2 integration, docs/loader_design.md section 8 M2). Registers: none of the probed
// functions reads eax/edx (or ecx, except as `this`) before writing it, so the host frame in between
// is transparent. Rule of INTERFACES.md 4.14: no lock or owning object is held across a guest call.
//
// The only non-observer is trap:Game::DiscErrorScreen: the original vf0 0xF2FD0 shows the disc error
// screen in an endless loop, so a call means a failed read (M2 exit 5) and ends the process at once.
#include "xapi/game_probes.h"

#include "core/crash.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "core/trace.h"

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace xapi {
namespace {

constexpr std::uint32_t kMaxGuestName = 128;  // longest guest string copied into the log
constexpr std::size_t kMaxArchives = 128;     // FutPAL00..99 + sysPAL/sysNTSC
constexpr std::size_t kMaxContexts = 32;      // context list walk / history cap

std::atomic<std::uint32_t> g_mounted{0};
std::atomic<std::uint32_t> g_mountFailures{0};
std::atomic<std::uint64_t> g_lookups{0};
std::atomic<std::uint64_t> g_hits{0};
std::atomic<std::uint64_t> g_frames{0};
std::atomic<std::uint32_t> g_contextSwitches{0};
std::atomic<std::uint32_t> g_discErrorCalls{0};
std::atomic<std::uint32_t> g_language{0};
std::atomic<std::uint32_t> g_languageCalls{0};
std::atomic<std::int32_t> g_videoStandard{-1};
std::atomic<bool> g_gameInitDone{false};
std::atomic<std::uint32_t> g_lastContext{0};
std::atomic<std::uint32_t> g_app{0};                 // CApp from Game::Init (0 = use g_pGame [0x2E7B84])
std::atomic<std::uint32_t> g_switchesAfterFrame{0};  // switches only the CApp::Frame fallback saw

// Archive names by mount index and the context history. Only the guest main thread writes them; the
// lock covers readers on other threads (exit reporter, tests). Never held across a guest call.
std::mutex g_lock;
std::vector<std::string>& ArchiveNames() {
    static std::vector<std::string> names;
    return names;
}
std::vector<std::string>& Contexts() {
    static std::vector<std::string> history;
    return history;
}

// Copies a NUL-terminated guest string (at most `max` bytes, stops at unreadable memory); bytes outside
// 0x20..0x7E and the quote are written as \xNN so that the log line stays one parsable line.
std::string GuestString(guest::GuestAddr p, std::uint32_t max = kMaxGuestName) {
    std::string out;
    if (p == 0) {
        return "(null)";
    }
    for (std::uint32_t i = 0; i < max; ++i) {
        // One VirtualQuery per page (at the start and whenever the string crosses a page boundary).
        if ((i == 0 || ((p + i) & 0xFFFu) == 0) && !guest::IsReadable(p + i, 1)) {
            out += "<unreadable>";
            break;
        }
        const auto c = guest::Read<unsigned char>(p + i);
        if (c == 0) {
            break;
        }
        if (c >= 0x20 && c < 0x7F && c != '"') {
            out += static_cast<char>(c);
        } else {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\x%02X", c);
            out += esc;
        }
    }
    return out;
}

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p != 0 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}

std::string ContextName(guest::GuestAddr context) {
    if (context == 0) {
        return "(none)";
    }
    return GuestString(ReadU32(context + guest::kGameContextNameOffset), 32);
}

std::string Caller(const void* returnAddress) {
    try {
        return guest::Symbolize(guest::Addr(returnAddress));
    } catch (...) {
        return "?";
    }
}

guest::GuestAddr Trampoline(guest::GuestAddr address) noexcept {
    const guest::GuestAddr t = core::HookTrampoline(address);
    if (t == 0) {
        core::Fatal("probe at 0x%08X runs without an installed trampoline", address);
    }
    return t;
}

// Records a context change from g_lastContext to `current` (context history + log line). `how` is
// appended after the frame number (empty for switches seen by probe:Game::ContextSwitch).
void NoteContextChange(guest::GuestAddr current, std::uint64_t frame, const char* how) {
    const guest::GuestAddr previous = g_lastContext.exchange(current);
    const std::string from = ContextName(previous);
    const std::string to = ContextName(current);
    g_contextSwitches.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (Contexts().size() < kMaxContexts) {
            Contexts().push_back(to);
        }
    }
    FUT_LOGI(Game, "game: context %s -> %s (CApp::Frame #%llu)%s", from.c_str(), to.c_str(),
             static_cast<unsigned long long>(frame), how);
}

void RegisterExitReporter() noexcept {
    core::AddExitReporter("game", &LogGameSummary);  // idempotent (same name)
}

// Archive entry i of the game's table: {+0 stream, +4 header}; 0 if out of range / unreadable.
guest::GuestAddr ArchiveEntry(std::uint32_t index) noexcept {
    const std::uint32_t count = ReadU32(guest::kGameArchiveCountVar);
    const guest::GuestAddr table = ReadU32(guest::kGameArchiveTableVar);
    if (index >= count || table == 0) {
        return 0;
    }
    return ReadU32(table + index * 4);
}

// Mount index of the archive whose stream is `stream` (the lookup returns entry+0), -1 if unknown.
int ArchiveIndexOfStream(guest::GuestAddr stream) noexcept {
    const std::uint32_t count = ReadU32(guest::kGameArchiveCountVar);
    for (std::uint32_t i = 0; i < count && i < kMaxArchives; ++i) {
        const guest::GuestAddr entry = ArchiveEntry(i);
        if (entry != 0 && ReadU32(entry) == stream) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string ArchiveName(int index) {
    std::lock_guard<std::mutex> lock(g_lock);
    const auto& names = ArchiveNames();
    if (index >= 0 && static_cast<std::size_t>(index) < names.size() && !names[index].empty()) {
        return names[index];
    }
    return "archive#" + std::to_string(index);
}

// ---- probes ---------------------------------------------------------------------------------------

// Archive mount FUN_0013FE70 - cdecl 1 (name; NULL = "unmount", no effect), plain ret 0x13FFAC,
// returns al = 1 on success. Called through the jmp thunk 0x13FFB0 by CApp::Init 0xD26A0 for
// sysPAL.img (else sysNTSC.img) and FutPAL%02d.img 00..99 until the first failure (else FutNTSC%02d),
// and with NULL by CApp::Shutdown 0xD2920 (files.md 3.2, startup.md 2.6 #4/#7).
// On success the game appends an 8-byte entry {stream, header} to [0x2D7AF0] (count [0x2D7AF8]).
// Prologue 6A FF 68 D6 CE 17 00 = push -1; push 0x17CED6 (SEH frame setup).
std::uint32_t __cdecl ArchiveMountProbe(const char* name) noexcept {
    const std::uint32_t countBefore = ReadU32(guest::kGameArchiveCountVar);
    const std::uint32_t eax = core::CallGuest(Trampoline(guest::kGameArchiveMount), {guest::Addr(name)});
    try {
        RegisterExitReporter();
        if (name == nullptr) {
            FUT_LOGD(Vfs, "archive: FUN_0013FE70(NULL) (unmount request, no effect in this build)");
            return eax;
        }
        const std::string text = GuestString(guest::Addr(name));
        const std::uint32_t count = ReadU32(guest::kGameArchiveCountVar);
        if ((eax & 0xFF) != 0 && count == countBefore + 1) {
            const std::uint32_t index = count - 1;
            const guest::GuestAddr entry = ArchiveEntry(index);
            const guest::GuestAddr stream = ReadU32(entry);
            const guest::GuestAddr header = ReadU32(entry + 4);
            const std::uint32_t headerSize = ReadU32(header);
            // NiUDSStream +0x40 = file size (files.md 2.4).
            const std::uint32_t fileSize = ReadU32(stream + 0x40);
            {
                std::lock_guard<std::mutex> lock(g_lock);
                auto& names = ArchiveNames();
                if (names.size() <= index && index < kMaxArchives) {
                    names.resize(index + 1);
                }
                if (index < names.size()) {
                    names[index] = text;
                }
            }
            g_mounted.fetch_add(1);
            FUT_LOGI(Vfs, "archive: mounted %s as #%u (%u bytes, header 0x%X bytes, stream 0x%08X)",
                     text.c_str(), index, fileSize, headerSize, stream);
            core::trace::VfsLine("%-25s \"%s\" -> archive #%u (%u bytes, header 0x%X) : mounted",
                                 "archive mount", text.c_str(), index, fileSize, headerSize);
        } else {
            g_mountFailures.fetch_add(1);
            FUT_LOGI(Vfs, "archive: %s not mounted (FUN_0013FE70 -> %u; the game stops this mount loop here)",
                     text.c_str(), eax & 0xFF);
            core::trace::VfsLine("%-25s \"%s\" : not mounted (see the open above)", "archive mount",
                                 text.c_str());
        }
    } catch (...) {
    }
    return eax;
}

// Archive lookup FUN_0013FE10 - cdecl 3 (name, &offset, &size), plain ret; returns the archive stream
// of the first mounted archive that contains `name` (case-insensitive, '/' = '\'), else 0; returns 0 at
// once while [0x2D7AE8] = 0 (during a mount). Only caller: NiUDSStream ctor 0x13F3B2 (&offset =
// stream +0x4C, &size = +0x40), which opens "D:\" + name as a loose file when it gets 0
// (files.md 2.4/3.3). Prologue A0 E8 7A 2D 00 = mov al,[0x2D7AE8] (absolute operand).
std::uint32_t __cdecl ArchiveLookupProbe(const char* name, std::uint32_t* offset,
                                         std::uint32_t* size) noexcept {
    const bool enabled = guest::Read<std::uint8_t>(guest::kGameArchiveSearchVar) != 0;
    const std::uint32_t stream = core::CallGuest(Trampoline(guest::kGameArchiveLookup),
                                                 {guest::Addr(name), guest::Addr(offset), guest::Addr(size)});
    if (!enabled) {
        return stream;
    }
    try {
        g_lookups.fetch_add(1);
        const std::string text = GuestString(guest::Addr(name));
        if (stream != 0) {
            g_hits.fetch_add(1);
            const int index = ArchiveIndexOfStream(stream);
            const std::string archive = ArchiveName(index);
            const std::uint32_t off = ReadU32(guest::Addr(offset));
            const std::uint32_t len = ReadU32(guest::Addr(size));
            core::trace::VfsLine("%-25s \"%s\" -> %s+0x%08X (%u bytes) : hit archive #%d", "archive lookup",
                                 text.c_str(), archive.c_str(), off, len, index);
            FUT_LOGT(Vfs, "archive lookup \"%s\" -> %s+0x%08X (%u bytes)", text.c_str(), archive.c_str(), off,
                     len);
        } else {
            core::trace::VfsLine("%-25s \"%s\" : not archived (loose file D:\\ follows)", "archive lookup",
                                 text.c_str());
            FUT_LOGT(Vfs, "archive lookup \"%s\" -> not archived", text.c_str());
        }
    } catch (...) {
    }
    return stream;
}

// Game::Init FUN_000A26A0 - thiscall (ecx = CApp), no stack args, plain ret 0xA2C1A; eax = 1 on
// success. Called once at the end of CApp::Init 0xD26A0 (`mov ecx,esi; call` 0xD28F6). Sets fps/dt/
// movie tag from CApp+0xE8 (0xA26BD..0xA2764), selects the language again in PAL mode (0xA28DB..
// 0xA2900: FUN_000D9990 -> XGetLanguage, FUN_000D9B20 stores it at [0x2EC640]+0x18), creates the
// contexts Intro/Frontend/Attract/Ingame/Death, enters the first one and reads BUILD.TXT into 0x2E7A68
// (0xA2B8E..0xA2BD2). Evidence: startup.md 2.6 #11-#19, files.md 1.2/3.2. Declared __fastcall with an
// unused edx (same layout and purge as thiscall without stack args).
// Prologue 64 A1 00 00 00 00 = mov eax,fs:[0] (fs:[0] is the SEH chain, never fs-patched).
std::uint32_t __fastcall GameInitProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kGameInit), self, {});
    try {
        RegisterExitReporter();
        g_app.store(self);
        const std::uint32_t standard = ReadU32(self + guest::kCAppVideoStandardOffset);
        g_videoStandard.store(static_cast<std::int32_t>(standard));
        g_gameInitDone.store(true);
        float fps = 0.0f, dt = 0.0f;
        std::memcpy(&fps, guest::Ptr<const void>(guest::kGameFpsVar), sizeof(fps));
        std::memcpy(&dt, guest::Ptr<const void>(guest::kGameFrameTimeVar), sizeof(dt));
        FUT_LOGI(Game, "game: Game::Init (0x%08X) -> %u (CApp 0x%08X)", guest::kGameInit, eax & 0xFF, self);
        FUT_LOGI(
            Game,
            "game: video standard CApp+0xE8 = %u (%s; %.0f fps, fixed time step %.4f s, movie tag \"%s\")",
            standard,
            standard == 0   ? "PAL"
            : standard == 1 ? "NTSC"
                            : "?",
            static_cast<double>(fps), static_cast<double>(dt),
            GuestString(guest::kGameMovieTagVar, 8).c_str());
        const guest::GuestAddr strings = ReadU32(guest::kGameStringManagerPtr);
        const std::string language =
            strings != 0 ? GuestString(ReadU32(strings + guest::kStringManagerLanguageOffset), 32)
                         : "(no string manager)";
        const std::uint32_t xlang = g_language.load();
        FUT_LOGI(Game, "game: language \"%s\" (string manager 0x%08X; XGetLanguage() = %u %s)",
                 language.c_str(), strings, xlang, XboxLanguageName(xlang));
        FUT_LOGI(Game, "game: BUILD.TXT = \"%s\"",
                 GuestString(guest::kGameBuildText, guest::kGameBuildTextSize).c_str());
        std::string archives;
        std::uint32_t archiveCount = 0;
        for (const std::string& n : MountedArchiveNames()) {
            archives += (archives.empty() ? "" : ", ") + n;
            ++archiveCount;
        }
        FUT_LOGI(Game, "game: archives mounted: %s (%u; game table count %u)", archives.c_str(), archiveCount,
                 ReadU32(guest::kGameArchiveCountVar));
        std::string contexts;
        guest::GuestAddr c = ReadU32(self + guest::kCAppContextListOffset);
        for (std::size_t i = 0; c != 0 && i < kMaxContexts; ++i) {
            contexts += (contexts.empty() ? "" : ", ") + ContextName(c);
            c = ReadU32(c + guest::kGameContextNextOffset);
        }
        const guest::GuestAddr current = ReadU32(self + guest::kCAppCurrentContextOffset);
        g_lastContext.store(current);
        const std::string currentName = ContextName(current);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            Contexts().push_back(currentName);
        }
        FUT_LOGI(Game, "game: contexts %s; current \"%s\" (0x%08X)", contexts.c_str(), currentName.c_str(),
                 current);
        const guest::GuestAddr handler = ReadU32(guest::kGameDiscErrorHandlerVar);
        FUT_LOGI(Game, "game: disc error handler [0x%08X] = 0x%08X (vtable 0x%08X, vf0 0x%08X%s)",
                 guest::kGameDiscErrorHandlerVar, handler, ReadU32(handler), ReadU32(ReadU32(handler)),
                 ReadU32(ReadU32(handler)) == guest::kGameDiscErrorScreen ? " = trap:Game::DiscErrorScreen"
                                                                          : "");
    } catch (...) {
    }
    return eax;
}

// CApp::Frame FUN_000D2630 - thiscall (ecx = CApp), no stack args, plain ret 0xD265A/0xD268B; al =
// FALSE ends the main loop of main (Game::Frame 0xA2C20 had no context left). Called once per frame
// (startup.md 3.1). The probe counts frames. Context switches are logged by probe:Game::ContextSwitch
// where Game::Frame makes them; comparing CApp+0xBC after the frame is only the fallback (e.g. with that
// probe disabled), because it misses a context entered and left within one frame (review finding 4).
// Prologue 8B 15 40 67 2D 00 = mov edx,[0x2D6740] (absolute operand).
std::uint32_t __fastcall CAppFrameProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kCAppFrame), self, {});
    const std::uint64_t n = g_frames.fetch_add(1) + 1;
    try {
        const guest::GuestAddr current = ReadU32(self + guest::kCAppCurrentContextOffset);
        if (current != g_lastContext.load()) {
            g_switchesAfterFrame.fetch_add(1);
            NoteContextChange(current, n, " [seen after the frame]");
        }
        if ((eax & 0xFF) == 0) {
            FUT_LOGW(Game, "game: CApp::Frame #%llu returned FALSE - the main loop of main ends (context %s)",
                     static_cast<unsigned long long>(n), ContextName(current).c_str());
        }
    } catch (...) {
    }
    return eax;
}

// FUN_000A48C0 - cdecl, no args, plain ret 0xA48D6, returns 0: maintenance of the game memory pool
// 0x2E7D60. Game::Frame calls it at 0xA2E40 right after a context switch (cur->Exit(), FUN_0002A660,
// CApp+0xBC = CApp+0xC0, CApp+0xC0 = 0) and before the new context's Enter(); the level loader
// FUN_0002A760 calls it too (startup.md 3.2, guest.h kGamePoolMaintenance). The probe logs the switch
// only for the Game::Frame call site (return address 0xA2E45), reading the new context from CApp+0xBC.
// Unlike the CApp::Frame comparison it also sees a context that is left again within the same frame
// (review finding 4: Frontend -> Attract -> Frontend when the XMV stub ends Attract-PAL.xmv in its first
// Update). The frame number is the CApp::Frame call in progress.
// Prologue B9 60 7D 2E 00 = mov ecx,0x2E7D60 (immediate operand; the function writes ecx first and does
// not read eax/edx).
std::uint32_t __cdecl ContextSwitchProbe() noexcept {
    if (guest::Addr(_ReturnAddress()) == guest::kGameFrameContextSwitchReturn) {
        try {
            guest::GuestAddr app = g_app.load();
            if (app == 0) {
                app = ReadU32(guest::kGameGamePtr);
            }
            NoteContextChange(ReadU32(app + guest::kCAppCurrentContextOffset), g_frames.load() + 1, "");
        } catch (...) {
        }
    }
    return core::CallGuest(Trampoline(guest::kGamePoolMaintenance));
}

// XAPILIB::XGetLanguage 0xF6440 - no arguments, plain ret (0xF6475/0xF6479): ExQueryNonVolatileSetting(7)
// (HLE: [settings] language), values > 9 replaced by the region default. Only caller FUN_000D9990
// (0xD9990), which maps 3 german, 4 french, 5 spanish, 6 italian, else english (files.md 3.2).
// M2 exit 2: with [settings] language = german it must return 3. Logged when the value changes.
// Prologue 55 8B EC 51 51 = push ebp; mov ebp,esp; push ecx; push ecx.
std::uint32_t __stdcall XGetLanguageProbe() noexcept {
    const std::uint32_t value = core::CallGuest(Trampoline(guest::kXapiXGetLanguage));
    g_languageCalls.fetch_add(1);
    if (g_language.exchange(value) != value) {
        try {
            FUT_LOGI(Xapi, "XGetLanguage() = %u (%s), called from %s", value, XboxLanguageName(value),
                     Caller(_ReturnAddress()).c_str());
        } catch (...) {
        }
    }
    return value;
}

// Disc error screen = vf0 0xF2FD0 of the disc error handler object 0x2D707C (thiscall, 1 stack arg:
// the failing NiUDSStream; called through DAT_0030E68C at 0x13F1EE (ReadFileEx completion with
// dwErr != 0) and 0x13F6BD (ReadFileEx returned FALSE); files.md 2.4). The original draws the disc
// error message in an endless loop (jmp 0xF3004 at 0xF30A4, no ret), i.e. the game hangs. M2 exit 5:
// never called. The trap logs TRAP + the stream and ends the process with core::Fatal (exit code 3);
// `[hooks] disable = trap:Game::DiscErrorScreen` restores the original endless loop.
// Not a Ghidra function: tools/gen/extra_function_starts.tsv (0xF2FD0, 0xD9 bytes).
void __fastcall DiscErrorScreenTrap(std::uint32_t self, std::uint32_t /*edx*/,
                                    std::uint32_t stream) noexcept {
    g_discErrorCalls.fetch_add(1);
    const std::uint32_t handle = ReadU32(stream + 0x14);  // NiUDSStream layout (files.md 2.4)
    const std::uint32_t base = ReadU32(stream + 0x4C);
    const std::uint32_t pos = ReadU32(stream + 0x5C);
    const std::uint32_t fileSize = ReadU32(stream + 0x40);
    FUT_LOGE(Game,
             "TRAP DiscErrorScreen @0x%08X from %s (handler 0x%08X, stream 0x%08X: handle 0x%X, archive base "
             "0x%X, "
             "position 0x%X, size 0x%X) - a disc read failed (ReadFileEx returned FALSE or completed with an "
             "error)",
             guest::kGameDiscErrorScreen, Caller(_ReturnAddress()).c_str(), self, stream, handle, base, pos,
             fileSize);
    core::Fatal("disc error handler called (the original shows the disc error screen forever; M2 exit 5)");
}

}  // namespace

const char* XboxLanguageName(std::uint32_t value) noexcept {
    static constexpr const char* kNames[] = {"?",       "english", "japanese", "german",  "french",
                                             "spanish", "italian", "korean",   "chinese", "portuguese"};
    return value < std::size(kNames) ? kNames[value] : "?";
}

GameObserverStats GetGameObserverStats() noexcept {
    GameObserverStats s;
    s.archivesMounted = g_mounted.load();
    s.archiveMountFailures = g_mountFailures.load();
    s.archiveLookups = g_lookups.load();
    s.archiveHits = g_hits.load();
    s.frames = g_frames.load();
    s.contextSwitches = g_contextSwitches.load();
    s.switchesSeenAfterFrame = g_switchesAfterFrame.load();
    s.discErrorCalls = g_discErrorCalls.load();
    s.language = g_language.load();
    s.languageCalls = g_languageCalls.load();
    s.videoStandard = g_videoStandard.load();
    s.gameInitDone = g_gameInitDone.load();
    return s;
}

std::vector<std::string> MountedArchiveNames() {
    std::lock_guard<std::mutex> lock(g_lock);
    return ArchiveNames();
}

std::vector<std::string> ContextHistory() {
    std::lock_guard<std::mutex> lock(g_lock);
    return Contexts();
}

void LogGameSummary() noexcept {
    try {
        const GameObserverStats s = GetGameObserverStats();
        std::string history;
        for (const std::string& c : ContextHistory()) {
            history += (history.empty() ? "" : " -> ") + c;
        }
        FUT_LOGI(Game,
                 "game summary: %llu CApp::Frame calls, contexts %s (%u switches, %u of them seen only "
                 "after the frame), video standard %d, "
                 "XGetLanguage %u "
                 "(%s, %u calls), archives %u mounted / %u not mounted, archive lookups %llu (%llu hits), "
                 "disc error "
                 "screen calls %u",
                 static_cast<unsigned long long>(s.frames), history.empty() ? "(none)" : history.c_str(),
                 s.contextSwitches, s.switchesSeenAfterFrame, s.videoStandard, s.language,
                 XboxLanguageName(s.language), s.languageCalls, s.archivesMounted, s.archiveMountFailures,
                 static_cast<unsigned long long>(s.archiveLookups),
                 static_cast<unsigned long long>(s.archiveHits), s.discErrorCalls);
    } catch (...) {
    }
}

}  // namespace xapi

FUT_PROBE(guest::kGameArchiveMount, &xapi::ArchiveMountProbe, Cdecl, "probe:Game::ArchiveMount",
          "6A FF 68 D6 CE 17 00", 7);
FUT_PROBE(guest::kGameArchiveLookup, &xapi::ArchiveLookupProbe, Cdecl, "probe:Game::ArchiveLookup",
          "A0 E8 7A 2D 00", 5);
FUT_PROBE(guest::kGameInit, &xapi::GameInitProbe, Thiscall, "probe:Game::Init", "64 A1 00 00 00 00", 6);
FUT_PROBE(guest::kCAppFrame, &xapi::CAppFrameProbe, Thiscall, "probe:CApp::Frame", "8B 15 40 67 2D 00", 6);
FUT_PROBE(guest::kGamePoolMaintenance, &xapi::ContextSwitchProbe, Cdecl, "probe:Game::ContextSwitch",
          "B9 60 7D 2E 00", 5);
FUT_PROBE(guest::kXapiXGetLanguage, &xapi::XGetLanguageProbe, Stdcall, "probe:XAPILIB::XGetLanguage",
          "55 8B EC 51 51", 5);
FUT_HOOK_EX(guest::kGameDiscErrorScreen, &xapi::DiscErrorScreenTrap, Thiscall, "trap:Game::DiscErrorScreen",
            "83 EC 0C 8B 0D 84 7B 2E 00", true);
