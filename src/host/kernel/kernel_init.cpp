// Kernel HLE initialization (HostMain stage 3, src/INTERFACES.md 3). Owned by the kernel agent.
#include "kernel/kernel_init.h"

#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_internal.h"
#include "kernel/kernel_settings.h"

#include <string>

namespace krnl {

bool StageInitKernel(const core::Config& config) {
    KernelSettings settings;
    std::string error;
    if (!LoadKernelSettings(config, settings, error)) {
        FUT_LOGE(Krnl, "kernel configuration: %s", error.c_str());
        return false;
    }
    SetSettings(settings);
    InitIrql();
    if (!InitThreading()) {
        FUT_LOGE(Krnl, "kernel init: cannot create the guest-thread exit event (%lu)", GetLastError());
        return false;
    }
    if (!InitDataExports(settings)) {
        return false;
    }
    const char* language = LanguageName(settings.language);
    FUT_LOGI(Krnl,
             "kernel settings: language %u (%s), game region %u, AV region 0x%08X, video flags 0x%X, "
             "audio flags 0x%X, AV pack %u, min guest stack 0x%X",
             settings.language, language ? language : "?", settings.gameRegion, settings.avRegion,
             settings.videoFlags, settings.audioFlags, settings.avPack, settings.minGuestStack);
    LogDataExports();
    if (!ValidateRegistry()) {
        FUT_LOGE(Krnl, "kernel export registry is inconsistent");
        return false;
    }
    std::uint32_t impl = 0;
    for (std::uint32_t s = 0; s < gen::kSlotCount; ++s) {
        impl += IsImplemented(s) ? 1u : 0u;
    }
    FUT_LOGI(Krnl, "stage InitKernel: %u of %u kernel slots have a registered implementation", impl,
             gen::kSlotCount);
    return true;
}

}  // namespace krnl
