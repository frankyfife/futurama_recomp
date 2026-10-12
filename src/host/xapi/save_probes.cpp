// Save-game observer probes and the `game level_end` debug action (M7; see save_probes.h for the list).
//
// Evidence (analysis/loader/m7_saves.md 2-4, files.md 4.1/4.2): the save layer of the game (0xE0040..0xE1500)
// uses only XAPI (XCreateSaveGame, CreateFileA, ReadFile, WriteFileEx, CopyFileA, XFindFirst/NextSaveGame,
// XDeleteSaveGame, XCalculateSignature*), which runs natively on the kernel HLE. Nothing here changes guest
// behaviour: every probe calls the original through its trampoline and returns its eax. Prologues and returns
// were checked with capstone on orig/retail_eu/default.xbe (whole, position-independent instructions; a raw
// scan found no E8/E9/Jcc into a stolen range). Registers: none of the probed functions reads eax/edx before
// writing it, ecx only as `this`. Rule of INTERFACES.md 4.14: no lock is held across a guest call.
#include "xapi/save_probes.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace xapi {
namespace {

constexpr std::uint32_t kContainerSize = 0x1000;    // files.md 4.2: every container is exactly 4096 bytes
constexpr std::uint32_t kMaxLoggedPayload = 0x200;  // characters of Futurama.dat text in one log line
constexpr std::uint32_t kMaxGuestPath = 260;

std::atomic<std::uint32_t> g_creates{0};
std::atomic<std::uint32_t> g_createFailures{0};
std::atomic<std::uint32_t> g_writes{0};
std::atomic<std::uint32_t> g_writeFailures{0};
std::atomic<std::uint32_t> g_reads{0};
std::atomic<std::uint32_t> g_readFailures{0};
std::atomic<std::uint32_t> g_signaturesOk{0};
std::atomic<std::uint32_t> g_signaturesBad{0};
std::atomic<std::uint32_t> g_deletes{0};
std::atomic<std::uint32_t> g_deleteFailures{0};
std::atomic<std::uint32_t> g_saveMenuOpened{0};
std::atomic<std::uint32_t> g_dialogs{0};
std::atomic<bool> g_levelEndPending{false};
// Next level name for FsChangeLevel. The game copies it (FUN_000A2340: strncpy 8 into CApp+0xAC), so a host
// buffer is fine (32-bit process: host addresses are valid guest pointers). Written before g_levelEndPending
// is set.
char g_nextLevel[16] = {};

// Path of the last container ReadContainer read (VerifySignature has only the buffer). Guest main thread
// only; the lock covers the exit reporter.
std::mutex g_lock;
std::string& LastReadPath() {
    static std::string path;
    return path;
}

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p != 0 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}

guest::GuestAddr Trampoline(guest::GuestAddr address) noexcept {
    const guest::GuestAddr t = core::HookTrampoline(address);
    if (t == 0) {
        core::Fatal("probe at 0x%08X runs without an installed trampoline", address);
    }
    return t;
}

void RegisterExitReporter() noexcept {
    core::AddExitReporter("save", &LogSaveSummary);  // idempotent (same name)
}

// Printable copy of guest bytes: 0x20..0x7E except the quote as is, '\r' '\n' as "\r" "\n", others as \xNN.
std::string Printable(const unsigned char* p, std::uint32_t n) {
    std::string out;
    for (std::uint32_t i = 0; i < n; ++i) {
        const unsigned char c = p[i];
        if (c >= 0x20 && c < 0x7F && c != '"') {
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\x%02X", c);
            out += esc;
        }
    }
    return out;
}

// NUL-terminated guest ANSI string (at most `max` bytes).
std::string GuestAnsi(guest::GuestAddr p, std::uint32_t max = kMaxGuestPath) {
    if (p == 0) {
        return "(null)";
    }
    std::string out;
    for (std::uint32_t i = 0; i < max; ++i) {
        if ((i == 0 || ((p + i) & 0xFFFu) == 0) && !guest::IsReadable(p + i, 1)) {
            return out + "<unreadable>";
        }
        const auto c = guest::Read<unsigned char>(p + i);
        if (c == 0) {
            break;
        }
        out += static_cast<char>(c);
    }
    return Printable(reinterpret_cast<const unsigned char*>(out.data()),
                     static_cast<std::uint32_t>(out.size()));
}

// NUL-terminated guest UTF-16 string -> UTF-8 (at most `max` characters; the game's save names are
// ASCII/Latin-1 in practice: "Futurama Options", "Futurama Extras", the name typed in the save menu).
std::string GuestWide(guest::GuestAddr p, std::uint32_t max = 128) {
    if (p == 0) {
        return "(null)";
    }
    std::wstring w;
    for (std::uint32_t i = 0; i < max; ++i) {
        const guest::GuestAddr a = p + 2 * i;
        if ((i == 0 || (a & 0xFFFu) < 2) && !guest::IsReadable(a, 2)) {
            break;
        }
        const auto c = guest::Read<std::uint16_t>(a);
        if (c == 0) {
            break;
        }
        w += static_cast<wchar_t>(c);
    }
    std::string utf8 = core::WideToUtf8(w);
    return Printable(reinterpret_cast<const unsigned char*>(utf8.data()),
                     static_cast<std::uint32_t>(utf8.size()));
}

bool EndsWithFuturamaDat(const std::string& path) noexcept {
    static constexpr char kName[] = "futurama.dat";
    constexpr std::size_t n = sizeof(kName) - 1;
    if (path.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const char c = path[path.size() - n + i];
        if (static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) != kName[i]) {
            return false;
        }
    }
    return true;
}

// Payload text of a Futurama.dat container (u32 length + ASCII "Key=Value" lines, files.md 4.1/4.2).
std::string GamePayloadText(guest::GuestAddr container) {
    if (container == 0 || !guest::IsReadable(container, kContainerSize)) {
        return "<unreadable>";
    }
    std::uint32_t len = guest::Read<std::uint32_t>(container);
    if (len > kContainerSize - 4 - 0x14) {
        return "<payload length 0x" + std::to_string(len) + " out of range>";
    }
    const auto* text = guest::Ptr<const unsigned char>(container + 4);
    std::uint32_t shown = len;
    while (shown > 0 && text[shown - 1] == 0) {
        --shown;  // the game counts the terminating NUL
    }
    std::string out = Printable(text, shown < kMaxLoggedPayload ? shown : kMaxLoggedPayload);
    if (shown > kMaxLoggedPayload) {
        out += "...";
    }
    return out;
}

const char* DispositionName(std::uint32_t d) noexcept {
    switch (d) {
    case 1:
        return "CREATE_NEW";
    case 2:
        return "CREATE_ALWAYS";
    case 3:
        return "OPEN_EXISTING";
    case 4:
        return "OPEN_ALWAYS";
    default:
        return "?";
    }
}

const char* ReadResultName(std::uint32_t r) noexcept {
    switch (r) {
    case 0:
        return "wrong size (not 4096 bytes)";
    case 1:
        return "read error";
    case 2:
        return "missing";
    case 3:
        return "ok";
    default:
        return "?";
    }
}

const char* IconName(std::uint32_t icon) noexcept {
    switch (icon) {
    case 0:
        return "saveimage.xbx";
    case 1:
        return "options_saveimage.xbx";
    case 2:
        return "extras_saveimage.xbx";
    default:
        return "?";
    }
}

// ---- probes
// ---------------------------------------------------------------------------------------------------

// XAPILIB::XCreateSaveGame 0xF6987 (stdcall 6, ret 0x18; guest.h kXapiXCreateSaveGame). Callers:
// WriteContainer 0xE0B00 (OPEN_ALWAYS) and the icon copy FUN_000E0420 (OPEN_EXISTING). Creates/opens U:\<12
// hex digits>\ with SaveMeta.xbx (files.md 4.1); pathOut receives "U:\<dir>\".
std::uint32_t __stdcall XCreateSaveGameProbe(const char* root, const wchar_t* name, std::uint32_t disposition,
                                             std::uint32_t flags, char* pathOut,
                                             std::uint32_t cchPath) noexcept {
    const std::uint32_t eax = core::CallGuest(
        Trampoline(guest::kXapiXCreateSaveGame),
        {guest::Addr(root), guest::Addr(name), disposition, flags, guest::Addr(pathOut), cchPath});
    try {
        RegisterExitReporter();
        g_creates.fetch_add(1);
        if (eax != 0) {
            g_createFailures.fetch_add(1);
        }
        FUT_LOGI(Xapi, "save: XCreateSaveGame(%s, \"%s\", %s, flags %u) -> %u %s",
                 GuestAnsi(guest::Addr(root)).c_str(), GuestWide(guest::Addr(name)).c_str(),
                 DispositionName(disposition), flags, eax,
                 eax == 0 ? GuestAnsi(guest::Addr(pathOut)).c_str() : "(failed)");
    } catch (...) {
    }
    return eax;
}

// XAPILIB::XDeleteSaveGame 0xF6BEA (stdcall 2, ret 8; guest.h kXapiXDeleteSaveGame). Only caller:
// FUN_000E0740 (delete from the load menu with Y, and "delete damaged save game?" -> Yes). Checks the name in
// SaveMeta.xbx and removes the directory tree (FUN_000F8709).
std::uint32_t __stdcall XDeleteSaveGameProbe(const char* root, const wchar_t* name) noexcept {
    const std::uint32_t eax =
        core::CallGuest(Trampoline(guest::kXapiXDeleteSaveGame), {guest::Addr(root), guest::Addr(name)});
    try {
        RegisterExitReporter();
        g_deletes.fetch_add(1);
        if (eax != 0) {
            g_deleteFailures.fetch_add(1);
        }
        FUT_LOGI(Xapi, "save: XDeleteSaveGame(%s, \"%s\") -> %u%s", GuestAnsi(guest::Addr(root)).c_str(),
                 GuestWide(guest::Addr(name)).c_str(), eax, eax == 0 ? " (directory removed)" : " (FAILED)");
    } catch (...) {
    }
    return eax;
}

// ReadContainer FUN_000E0210 (thiscall + 3, ret 0xC; guest.h kGameSaveReadContainer). Callers: LoadGame
// 0xE0690, LoadOptions 0xE0F80, LoadExtras 0xE1160.
std::uint32_t __fastcall ReadContainerProbe(std::uint32_t self, std::uint32_t /*edx*/, const char* path,
                                            std::uint32_t buffer, std::uint32_t bytes) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kGameSaveReadContainer), self,
                                                      {guest::Addr(path), buffer, bytes});
    try {
        RegisterExitReporter();
        const std::string text = GuestAnsi(guest::Addr(path));
        {
            std::lock_guard<std::mutex> lock(g_lock);
            LastReadPath() = text;
        }
        g_reads.fetch_add(1);
        if (eax != 3) {
            g_readFailures.fetch_add(1);
        }
        FUT_LOGI(Xapi, "save: read %s -> %s", text.c_str(), ReadResultName(eax));
    } catch (...) {
    }
    return eax;
}

// VerifySignature FUN_000E0340 (stdcall 2, ret 8; guest.h kGameSaveVerifySignature). Callers: LoadGame
// 0xE06B6, LoadOptions 0xE0FDF, LoadExtras 0xE11B7 - always right after ReadContainer returned 3.
std::uint32_t __stdcall VerifySignatureProbe(std::uint32_t container, std::uint32_t size) noexcept {
    const std::uint32_t eax = core::CallGuest(Trampoline(guest::kGameSaveVerifySignature), {container, size});
    try {
        RegisterExitReporter();
        const bool ok = (eax & 0xFF) != 0;
        (ok ? g_signaturesOk : g_signaturesBad).fetch_add(1);
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            path = LastReadPath();
        }
        FUT_LOGI(Xapi, "save: signature of %s %s", path.c_str(),
                 ok ? "ok" : "MISMATCH (the game reports it as damaged)");
        if (ok && size == kContainerSize && EndsWithFuturamaDat(path)) {
            FUT_LOGI(Xapi, "save: loaded game data: %s", GamePayloadText(container).c_str());
        }
    } catch (...) {
    }
    return eax;
}

// WriteContainer FUN_000E0B00 (thiscall + 3, ret 0xC; guest.h kGameSaveWriteContainer). Callers: SaveGame
// 0xE14B1
// ("Futurama.dat", name 0 = the save menu's choice), SaveOptions 0xE0F69 ("Options.dat", "Futurama Options"),
// SaveExtras 0xE1145 ("Unlock.dat", "Futurama Extras"). The signed container is save+0x28.
std::uint32_t __fastcall WriteContainerProbe(std::uint32_t self, std::uint32_t /*edx*/, const char* fileName,
                                             const wchar_t* saveName, std::uint32_t icon) noexcept {
    std::string file, payload;
    try {
        file = GuestAnsi(guest::Addr(fileName));
        if (EndsWithFuturamaDat(file)) {
            payload = GamePayloadText(ReadU32(self + guest::kSaveObjectContainerOffset));
        }
    } catch (...) {
    }
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kGameSaveWriteContainer), self,
                                                      {guest::Addr(fileName), guest::Addr(saveName), icon});
    try {
        RegisterExitReporter();
        const bool ok = (eax & 0xFF) != 0;
        g_writes.fetch_add(1);
        if (!ok) {
            g_writeFailures.fetch_add(1);
        }
        // saveName 0: the name chosen in the save menu, [[[g_pApp]+0xF4]+4]+0x40 (0xE0B2A..0xE0B35).
        guest::GuestAddr name = guest::Addr(saveName);
        if (name == 0) {
            name =
                ReadU32(ReadU32(ReadU32(ReadU32(guest::kGameAppPtr) + guest::kCAppSaveUiOffset) + 4) + 0x40);
        }
        FUT_LOGI(Xapi, "save: write %s for \"%s\" (icon %s) -> %s", file.c_str(), GuestWide(name).c_str(),
                 IconName(icon), ok ? "ok" : "FAILED");
        if (ok && !payload.empty()) {
            FUT_LOGI(Xapi, "save: saved game data: %s", payload.c_str());
        }
    } catch (...) {
    }
    return eax;
}

// OpenSaveMenu FUN_000E4F70 (thiscall, no stack args, plain ret; guest.h kGameOpenSaveMenu).
std::uint32_t __fastcall OpenSaveMenuProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t eax = core::CallGuestThiscall(Trampoline(guest::kGameOpenSaveMenu), self, {});
    try {
        RegisterExitReporter();
        const bool ok = (eax & 0xFF) != 0;
        if (ok) {
            g_saveMenuOpened.fetch_add(1);
        }
        FUT_LOGI(Xapi, "save: save menu %s (called from 0x%08X, saving %s)", ok ? "opened" : "not opened",
                 guest::Addr(_ReturnAddress()),
                 guest::Read<std::uint8_t>(guest::kGameSavingEnabledVar) != 0 ? "enabled" : "disabled");
    } catch (...) {
    }
    return eax;
}

// SaveDialog ctor FUN_000DF750 (thiscall + 4, ret 0x10; guest.h kGameSaveDialogCtor): one line per message
// box of the save layer, e.g. "save: dialog MEMCARD_007 (MEMHINT_001)" = "Save successful" - input scripts
// wait for these.
std::uint32_t __fastcall SaveDialogProbe(std::uint32_t self, std::uint32_t /*edx*/, const char* text,
                                         const char* hint, std::uint32_t options,
                                         std::uint32_t value) noexcept {
    try {
        RegisterExitReporter();
        g_dialogs.fetch_add(1);
        FUT_LOGI(Xapi, "save: dialog %s (%s)",
                 text != nullptr ? GuestAnsi(guest::Addr(text), 32).c_str() : "-",
                 hint != nullptr ? GuestAnsi(guest::Addr(hint), 32).c_str() : "no hint");
    } catch (...) {
    }
    return core::CallGuestThiscall(Trampoline(guest::kGameSaveDialogCtor), self,
                                   {guest::Addr(text), guest::Addr(hint), options, value});
}

}  // namespace

SaveObserverStats GetSaveObserverStats() noexcept {
    SaveObserverStats s;
    s.creates = g_creates.load();
    s.createFailures = g_createFailures.load();
    s.writes = g_writes.load();
    s.writeFailures = g_writeFailures.load();
    s.reads = g_reads.load();
    s.readFailures = g_readFailures.load();
    s.signaturesOk = g_signaturesOk.load();
    s.signaturesBad = g_signaturesBad.load();
    s.deletes = g_deletes.load();
    s.deleteFailures = g_deleteFailures.load();
    s.saveMenuOpened = g_saveMenuOpened.load();
    s.dialogs = g_dialogs.load();
    return s;
}

void LogSaveSummary() noexcept {
    const SaveObserverStats s = GetSaveObserverStats();
    FUT_LOGI(
        Xapi,
        "save summary: %u XCreateSaveGame (%u failed), %u container writes (%u failed), %u container reads "
        "(%u failed), signatures %u ok / %u mismatch, %u deletes (%u failed), save menu opened %u times, %u "
        "save dialogs",
        s.creates, s.createFailures, s.writes, s.writeFailures, s.reads, s.readFailures, s.signaturesOk,
        s.signaturesBad, s.deletes, s.deleteFailures, s.saveMenuOpened, s.dialogs);
}

bool RequestLevelEnd(const char* nextLevel) noexcept {
    const std::size_t n = nextLevel != nullptr ? std::strlen(nextLevel) : 0;
    if (n == 0 || n > 8 || g_levelEndPending.load() || ReadU32(guest::kGameLevelPtr) == 0) {
        return false;
    }
    std::memcpy(g_nextLevel, nextLevel, n + 1);
    g_levelEndPending.store(true);
    return true;
}

void RunPendingGameActions() noexcept {
    if (!g_levelEndPending.exchange(false)) {
        return;
    }
    if (ReadU32(guest::kGameLevelPtr) == 0) {
        FUT_LOGW(Xapi, "save: debug action level_end dropped: no level is running");
        return;
    }
    // The level-script native FsChangeLevel (CID 0, FID 0xA1 in the native dispatcher FUN_000B0C40, call
    // 0xB369B): cdecl FUN_000B0520(const char* nextLevel).
    FUT_LOGI(Xapi, "save: debug action: level end via the script native FsChangeLevel 0x%08X(\"%s\")",
             guest::kGameNativeFsChangeLevel, g_nextLevel);
    core::CallGuest(guest::kGameNativeFsChangeLevel, {guest::Addr(g_nextLevel)});
}

}  // namespace xapi

FUT_PROBE(guest::kXapiXCreateSaveGame, &xapi::XCreateSaveGameProbe, Stdcall, "probe:XAPILIB::XCreateSaveGame",
          "55 8B EC 81 EC 58 02 00 00", 9);
FUT_PROBE(guest::kXapiXDeleteSaveGame, &xapi::XDeleteSaveGameProbe, Stdcall, "probe:XAPILIB::XDeleteSaveGame",
          "55 8B EC 81 EC 44 01 00 00", 9);
FUT_PROBE(guest::kGameSaveReadContainer, &xapi::ReadContainerProbe, Thiscall, "probe:Save::ReadContainer",
          "51 56 8B F1 8B 46 20", 7);
FUT_PROBE(guest::kGameSaveVerifySignature, &xapi::VerifySignatureProbe, Stdcall,
          "probe:Save::VerifySignature", "53 8B 5C 24 0C", 5);
FUT_PROBE(guest::kGameSaveWriteContainer, &xapi::WriteContainerProbe, Thiscall, "probe:Save::WriteContainer",
          "55 8B EC 83 E4 F8", 6);
FUT_PROBE(guest::kGameOpenSaveMenu, &xapi::OpenSaveMenuProbe, Thiscall, "probe:Save::OpenSaveMenu",
          "6A FF 68 4B 91 17 00", 7);
FUT_PROBE(guest::kGameSaveDialogCtor, &xapi::SaveDialogProbe, Thiscall, "probe:Save::Dialog",
          "6A FF 68 B9 8D 17 00", 7);
