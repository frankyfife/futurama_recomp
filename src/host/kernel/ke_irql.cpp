// IRQL emulation: KfLowerIrql, KfRaiseIrql, KeRaiseIrqlToDpcLevel (slots 0, 80, 1). Owned by the kernel
// agent.
//
// Design 1.7: the IRQL of a guest thread lives in TLS slot S_IRQL (the patched fs:[0x24] sites read
// its low byte, design 1.6) and a global "dispatcher lock" is held while a thread runs at
// IRQL >= DISPATCH_LEVEL (taken on the transition below -> at/above DISPATCH_LEVEL, released on the
// way back). Guest code relies on IRQL for mutual exclusion instead of lock prefixes (cpu.md 3).
// There is no DPC execution: D3D, DSOUND and XPP are replaced (design 1.7).
#include "core/guest.h"
#include "core/guest_thread.h"
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_hle.h"
#include "kernel/kernel_internal.h"

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#include <string>

namespace krnl {
namespace {

struct DispatcherLock {
    CRITICAL_SECTION cs;
    DispatcherLock() noexcept { InitializeCriticalSectionEx(&cs, 4000, CRITICAL_SECTION_NO_DEBUG_INFO); }
};

DispatcherLock& Lock() noexcept {
    static DispatcherLock lock;  // thread-safe magic static; trivially destructible (no atexit work)
    return lock;
}

inline void SetIrql(xbox::KIRQL irql) noexcept {
    core::SetGuestSlot(FUT_TLS_S_IRQL, irql);
}

// Applies the new IRQL and the dispatcher-lock transition.
void Transition(xbox::KIRQL oldIrql, xbox::KIRQL newIrql) noexcept {
    if (oldIrql < xbox::kIrqlDispatch && newIrql >= xbox::kIrqlDispatch) {
        EnterCriticalSection(&Lock().cs);
    }
    SetIrql(newIrql);
    if (oldIrql >= xbox::kIrqlDispatch && newIrql < xbox::kIrqlDispatch) {
        LeaveCriticalSection(&Lock().cs);
    }
}

std::string CallerSymbol(void* returnAddress) noexcept {
    try {
        return guest::Symbolize(guest::Addr(returnAddress));
    } catch (...) {
        return {};
    }
}

}  // namespace

void InitIrql() noexcept {
    (void)Lock();
}

xbox::KIRQL CurrentIrql() noexcept {
    return static_cast<xbox::KIRQL>(core::GetGuestSlot(FUT_TLS_S_IRQL) & 0xFF);
}

// KeRaiseIrqlToDpcLevel - slot 1, Ord 129, stdcall, 0 args (purge 0): KIRQL (old IRQL in al).
// Evidence: kernel_imports.tsv; kernel.md 4 (al read at 22/24 sites, e.g. 0x0F9C8C `mov cl,al`
// before KfLowerIrql in the shutdown routine sub_0f9c7f); design 1.7.
xbox::KIRQL __stdcall KeRaiseIrqlToDpcLevel() noexcept {
    const xbox::KIRQL old = CurrentIrql();
    if (old > xbox::kIrqlDispatch) {
        FUT_LOGE(Krnl, "KeRaiseIrqlToDpcLevel at IRQL %u (> DISPATCH_LEVEL) ret=%s", old,
                 CallerSymbol(_ReturnAddress()).c_str());
        return old;  // ASSUMPTION: never lower here (NT would bugcheck IRQL_NOT_GREATER_OR_EQUAL)
    }
    Transition(old, xbox::kIrqlDispatch);
    FUT_LOGT(Krnl, "KeRaiseIrqlToDpcLevel() %u -> 2", old);
    return old;
}
KRNL_FUNC(KeRaiseIrqlToDpcLevel, &KeRaiseIrqlToDpcLevel);

// KfRaiseIrql - slot 80, Ord 160, fastcall, NewIrql in cl: KIRQL (old IRQL in al).
// Evidence: kernel_imports.tsv; kernel.md 4 (2/2 sites set cl, e.g. 0x290AFA, and read al; only
// DSOUND/XPP). Raising to a lower IRQL is an error (NT: bugcheck 0x9) - logged and ignored.
xbox::KIRQL __fastcall KfRaiseIrql(xbox::KIRQL NewIrql) noexcept {
    const xbox::KIRQL old = CurrentIrql();
    if (NewIrql < old) {
        FUT_LOGE(Krnl, "KfRaiseIrql(%u) below the current IRQL %u ret=%s - ignored", NewIrql, old,
                 CallerSymbol(_ReturnAddress()).c_str());
        return old;
    }
    Transition(old, NewIrql);
    FUT_LOGT(Krnl, "KfRaiseIrql(%u) old %u", NewIrql, old);
    return old;
}
KRNL_FUNC(KfRaiseIrql, &KfRaiseIrql);

// KfLowerIrql - slot 0, Ord 161, fastcall, NewIrql in cl: VOID.
// Evidence: kernel_imports.tsv; kernel.md 4 (ecx set at 28/28 sites, e.g. 0x0F9C8C `mov cl,al`);
// design 1.7. Lowering to a higher IRQL is an error (NT: bugcheck 0xA) - logged and ignored.
void __fastcall KfLowerIrql(xbox::KIRQL NewIrql) noexcept {
    const xbox::KIRQL old = CurrentIrql();
    if (NewIrql > old) {
        FUT_LOGE(Krnl, "KfLowerIrql(%u) above the current IRQL %u ret=%s - ignored", NewIrql, old,
                 CallerSymbol(_ReturnAddress()).c_str());
        return;
    }
    Transition(old, NewIrql);
    FUT_LOGT(Krnl, "KfLowerIrql(%u) old %u", NewIrql, old);
}
KRNL_FUNC(KfLowerIrql, &KfLowerIrql);

}  // namespace krnl
