// Boot thread adoption + entry call (design 1.6 "Boot-Thread", 1.10 step 5). Owned by the core agent.
//
// entry 0xF84DD (startup.md 2.1, disassembly 0xF84DD..0xF854E): clamps the certificate size, computes
// TlsDataSize = 0x24 and _tls_index = -9, calls XAPILIB::CreateThread(NULL, 0, 0xF8469, NULL, 0, NULL)
// (-> HLE PsCreateSystemThreadEx creates the guest main thread), XapiBootToDash(1,1,0) on failure,
// CloseHandle(h) and returns with a plain `ret`. The boot thread then runs the message loop (1.7).
#include "core/boot.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/guest_thread.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "core/selftest.h"

#include <intrin.h>

#include <cstdlib>
#include <string>

namespace core {
namespace {

guest::GuestAddr g_stopAtAddress = 0;

// Target of the --stop-at detour (design 5.4 "Laufzeitschalter", Anhang B: main 0xD29B0). Reached
// through the 5-byte jmp at the guest function start, so _ReturnAddress() is the return address of
// the guest's call. Never returns; any calling convention works.
void __cdecl StopAtHook() noexcept {
    const guest::GuestAddr caller = guest::Addr(_ReturnAddress());
    std::string target, from;
    try {
        target = guest::Symbolize(g_stopAtAddress);
        from = guest::Symbolize(caller);
    } catch (...) {
    }
    FUT_LOGI(Core, "STOP-AT %s reached (called from %s)", target.c_str(), from.c_str());
    ExitHostProcess(kExitOk, "--stop-at reached");
}

bool ParseStopAt(const std::string& text, guest::GuestAddr& out) {
    if (_stricmp(text.c_str(), "main") == 0) {
        out = guest::kGameMain;
        return true;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != 0 || !guest::InXbeImage(v)) {
        return false;
    }
    out = static_cast<guest::GuestAddr>(v);
    return true;
}

}  // namespace

bool StageAdoptBootThreadAndRunEntry(const Config& config) {
    // Design 1.6: boot thread has KTHREAD.TlsData = 0 but a valid dummy TLS anchor.
    GuestThread boot{};
    if (!AdoptCurrentThread(0, "boot", &boot)) {
        return false;
    }
    if (RunSelfTests(SelfTestPhase::AdoptedThread) != 0) {
        FUT_LOGE(Core, "self-tests on the adopted boot thread failed - not starting the guest");
        return false;
    }
    if (!config.stopAt.empty()) {
        if (!ParseStopAt(config.stopAt, g_stopAtAddress)) {
            FUT_LOGE(Core, "--stop-at=%s: expected 'main' or a guest address 0x...", config.stopAt.c_str());
            return false;
        }
        const HookDesc desc{
            g_stopAtAddress, detail::CodeAddress(&StopAtHook), HookConv::Custom, "stop-at", nullptr, true,
            __FILE__};
        if (!InstallHookNow(desc)) {
            FUT_LOGE(Core, "--stop-at=%s: cannot hook 0x%08X", config.stopAt.c_str(), g_stopAtAddress);
            return false;
        }
    }
    if (config.GetBool("debug", "crash_test", false)) {
        // Verifies the crash path of design 1.11 (unhandled guest AV -> filter -> core::Fatal ->
        // ring buffer, backtraces, minidump, exit code 3): before entry _tls_index is still 0, so the
        // guest SetLastError reads a NULL TLS block pointer and writes to address 4 (0xF6EA1).
        FUT_LOGW(Core, "[debug] crash_test = 1: provoking an unhandled access violation in guest code");
        LogFlush();
        guest::Func<void(__stdcall*)(DWORD)>(guest::kXapiSetLastError)(0xDEAD);
    }
    FUT_LOGI(Core, "calling XBE entry 0x%08X on the boot thread (TLS anchor 0x%08X)", guest::kEntryPoint,
             boot.tlsAnchor);
    LogFlush();
    const std::uint32_t eax = fut_guest_call0(guest::kEntryPoint);
    FUT_LOGI(Core, "entry returned (eax=0x%08X); the boot thread continues with the message loop", eax);
    return true;
}

}  // namespace core
