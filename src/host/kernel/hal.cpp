// HAL exports: HalReturnToFirmware, HalRegisterShutdownNotification, HalInitiateShutdown
// (slots 55, 74, 78). Owned by the kernel agent.
#include "core/guest.h"
#include "core/host_main.h"
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"

#include <intrin.h>

#include <cstdio>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace krnl {
namespace {

// FIRMWARE_REENTRY (ASSUMPTION: names after the XDK; only values 2 and 4 occur, kernel.md 7.5).
const char* RoutineName(ULONG routine) noexcept {
    switch (routine) {
    case 0:
        return "HalHaltRoutine";
    case 1:
        return "HalRebootRoutine";
    case 2:
        return "HalQuickRebootRoutine";
    case 3:
        return "HalKdRebootRoutine";
    case 4:
        return "HalFatalErrorRebootRoutine";
    default:
        return "unknown routine";
    }
}

std::string CallerOf(void* returnAddress) noexcept {
    try {
        return guest::Symbolize(guest::Addr(returnAddress));
    } catch (...) {
        return {};
    }
}

}  // namespace

// HalReturnToFirmware - slot 55, Ord 49, stdcall, 1 arg (purge 4): VOID (FIRMWARE_REENTRY Routine),
// never returns. Evidence: kernel_imports.tsv; FUN_000f829c 0xF82D2 (XLaunchNewImageA path,
// Routine 2) and XapiBootToDash 0xF9028 (Routine 4, only for TitleID 0xFFFE0000); startup.md 3.5:
// XapiBootToDash is NOT noreturn, so the process must end here; design 1.10.
// Routine 4 is a fatal error reboot (core::Fatal, exit code 3); every other routine ends the
// process through core::ExitHostProcess with core::kExitGuestShutdown (4, "the guest ended itself").
void __stdcall HalReturnToFirmware(ULONG Routine) noexcept {
    const std::string caller = CallerOf(_ReturnAddress());
    if (Routine == 4) {
        core::Fatal("HalReturnToFirmware(4 = HalFatalErrorRebootRoutine) from %s", caller.c_str());
    }
    FUT_LOGI(Krnl, "HalReturnToFirmware(%lu = %s) from %s: the title ends (design 1.10)", Routine,
             RoutineName(Routine), caller.c_str());
    char reason[96];
    std::snprintf(reason, sizeof(reason), "HalReturnToFirmware(%lu = %s)", Routine, RoutineName(Routine));
    core::ExitHostProcess(core::kExitGuestShutdown, reason);
}
KRNL_FUNC(HalReturnToFirmware, &HalReturnToFirmware);

// HalRegisterShutdownNotification - slot 74, Ord 47, stdcall, 2 args (purge 8):
// VOID (PHAL_SHUTDOWN_REGISTRATION ShutdownRegistration, BOOLEAN Register).
// Evidence: kernel_imports.tsv; XapiInitProcess 0xF92C0: HalRegisterShutdownNotification(0x2D7530, 1),
// record {0xF9C7F (sub_0f9c7f), 0x80000000, 0, 0} (kernel.md 8.7, Verifikation); design 1.7 and
// Anhang A: no-op, so the shutdown routine (KeWait on IdexChannelObject) never runs.
void __stdcall HalRegisterShutdownNotification(const std::uint32_t* ShutdownRegistration,
                                               BOOLEAN Register) noexcept {
    std::uint32_t routine = 0, priority = 0;
    if (ShutdownRegistration != nullptr) {
        routine = ShutdownRegistration[0];
        priority = ShutdownRegistration[1];
    }
    FUT_LOGD(
        Krnl, "HalRegisterShutdownNotification(%p {routine 0x%08X, priority 0x%08X}, %u): no-op (design 1.7)",
        static_cast<const void*>(ShutdownRegistration), routine, priority, static_cast<unsigned>(Register));
}
KRNL_FUNC(HalRegisterShutdownNotification, &HalRegisterShutdownNotification);

// HalInitiateShutdown - slot 78, Ord 360, stdcall, 0 args (purge 0): VOID.
// Evidence: kernel_imports.tsv; only the AutoPowerDown DPC 0xF9B08 (if [0x310388] != 0), which never
// runs because KeSetTimer never fires (kernel.md 8.6); Anhang A "Defensiv": log only.
void __stdcall HalInitiateShutdown() noexcept {
    const std::string caller = CallerOf(_ReturnAddress());
    FUT_LOGW(Krnl, "HalInitiateShutdown() from %s: ignored (kernel.md 8.6)", caller.c_str());
}
KRNL_FUNC(HalInitiateShutdown, &HalInitiateShutdown);

}  // namespace krnl
