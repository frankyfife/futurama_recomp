// Guest thread adoption (design 1.6). Owned by the core agent.
//
// Evidence: cpu.md 2.2 (TLS anchor [S_TLS + (-9)*4] = [TlsData] = TLS block; KTHREAD +0x1C/+0x28/+0x12C;
// KPRCB +0x250 = 0; boot thread with TlsData = 0 needs a valid dummy anchor because CloseHandle's
// failure path reaches SetLastError), design 1.6 table.
#include "core/guest_thread.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"

#include <cstring>

namespace core {
namespace {

constexpr std::uint32_t kThreadBlockSize = 0x1000;  // KTHREAD at +0, TLS area at +0x200
constexpr std::uint32_t kTlsAreaOffset = 0x200;
static_assert(sizeof(xbox::FakeKThread) <= kTlsAreaOffset);

thread_local GuestThread t_thread{};
thread_local void* t_allocation = nullptr;

xbox::FakeKPrcb* AllocPrcb() noexcept {
    void* p = VirtualAlloc(nullptr, sizeof(xbox::FakeKPrcb), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return static_cast<xbox::FakeKPrcb*>(p);  // zero-filled by VirtualAlloc
}

bool BelowGuestLimit(const void* p, std::uint32_t size) noexcept {
    return p != nullptr && static_cast<std::uint64_t>(guest::Addr(p)) + size <= guest::kGuestMemoryLimit;
}

// Lowest address of the calling thread's stack reservation (TEB.DeallocationStack).
std::uint32_t CurrentStackReservationBase() noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    const volatile std::uint32_t probe = 0;
    if (VirtualQuery(const_cast<const std::uint32_t*>(&probe), &mbi, sizeof(mbi)) == 0) {
        return guest::Addr(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackLimit);
    }
    return guest::Addr(mbi.AllocationBase);
}

void SetSlots(const GuestThread& t, xbox::FakeKPrcb* prcb) noexcept {
    SetGuestSlot(FUT_TLS_S_TLS, t.tlsAnchor);
    SetGuestSlot(FUT_TLS_S_KTHREAD, guest::Addr(t.kthread));
    SetGuestSlot(FUT_TLS_S_PRCB, guest::Addr(prcb));
    SetGuestSlot(FUT_TLS_S_IRQL, xbox::kIrqlPassive);
    SetGuestSlot(FUT_TLS_S_DPC, 0);
}

void FillKThread(xbox::FakeKThread* kthread, std::uint32_t tlsData) noexcept {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    kthread->StackBase = guest::Addr(tib->StackBase);
    kthread->StackLimit = CurrentStackReservationBase();
    kthread->TlsData = tlsData;
    kthread->ThreadId = GetCurrentThreadId();
}

}  // namespace

xbox::FakeKPrcb* SharedFakePrcb() noexcept {
    static xbox::FakeKPrcb* prcb = AllocPrcb();
    return prcb;
}

std::uint32_t GetGuestSlot(FutTlsSlot slot) noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(TlsGetValue(TlsSlotIndex(slot))));
}

void SetGuestSlot(FutTlsSlot slot, std::uint32_t value) noexcept {
    TlsSetValue(TlsSlotIndex(slot), reinterpret_cast<void*>(static_cast<std::uintptr_t>(value)));
}

bool AdoptCurrentThread(std::uint32_t tlsDataSize, const char* threadName, GuestThread* out) noexcept {
    if (threadName != nullptr) {
        LogSetThreadName(threadName);
    }
    if (tlsDataSize > kThreadBlockSize - kTlsAreaOffset) {
        FUT_LOGE(Core, "AdoptCurrentThread: TlsDataSize 0x%X too large", tlsDataSize);
        return false;
    }
    xbox::FakeKPrcb* prcb = SharedFakePrcb();
    void* block = VirtualAlloc(nullptr, kThreadBlockSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (block == nullptr || prcb == nullptr) {
        FUT_LOGE(Core, "AdoptCurrentThread: VirtualAlloc failed (%lu)", GetLastError());
        if (block != nullptr) {
            VirtualFree(block, 0, MEM_RELEASE);
        }
        return false;
    }
    if (!BelowGuestLimit(block, kThreadBlockSize) || !BelowGuestLimit(prcb, sizeof(xbox::FakeKPrcb))) {
        FUT_LOGE(Core, "AdoptCurrentThread: guest-visible memory above 0x80000000 (design 1.2)");
        VirtualFree(block, 0, MEM_RELEASE);
        return false;
    }
    auto* bytes = static_cast<std::uint8_t*>(block);
    auto* kthread = reinterpret_cast<xbox::FakeKThread*>(bytes);
    std::uint8_t* tlsArea = bytes + kTlsAreaOffset;

    GuestThread t{};
    t.kthread = kthread;
    if (tlsDataSize > 0) {
        t.tlsData = tlsArea;
        FillKThread(kthread, guest::Addr(tlsArea));
        t.tlsAnchor = guest::Addr(tlsArea) + tlsDataSize;
    } else {
        // Boot thread: TlsData = 0, but a valid dummy TLS block behind the anchor:
        // [anchor + _tls_index*4] = [anchor - 0x24] = [dummy] = dummy + 4 (0x20-byte block, zeroed).
        t.tlsData = nullptr;
        FillKThread(kthread, 0);
        const std::uint32_t dummy = guest::Addr(tlsArea);
        guest::Write<std::uint32_t>(dummy, dummy + 4);
        t.tlsAnchor = dummy + xbox::kTlsDataSize;
    }
    t.threadId = kthread->ThreadId;
    SetSlots(t, prcb);
    t_thread = t;
    t_allocation = block;
    if (out != nullptr) {
        *out = t;
    }
    FUT_LOGD(Core, "adopted thread %lu: KTHREAD %p TlsData %p anchor 0x%08X stack 0x%08X..0x%08X", t.threadId,
             static_cast<void*>(kthread), static_cast<void*>(t.tlsData), t.tlsAnchor, kthread->StackLimit,
             kthread->StackBase);
    return true;
}

bool AdoptCurrentThreadWith(xbox::FakeKThread* kthread, std::uint8_t* tlsData, std::uint32_t tlsDataSize,
                            const char* threadName, GuestThread* out) noexcept {
    if (threadName != nullptr) {
        LogSetThreadName(threadName);
    }
    xbox::FakeKPrcb* prcb = SharedFakePrcb();
    if (prcb == nullptr || tlsDataSize < 4 || !BelowGuestLimit(kthread, sizeof(xbox::FakeKThread)) ||
        !BelowGuestLimit(tlsData, tlsDataSize)) {
        FUT_LOGE(Core,
                 "AdoptCurrentThreadWith: invalid memory (KTHREAD %p, TlsData %p size 0x%X; must be below "
                 "0x80000000)",
                 static_cast<void*>(kthread), static_cast<void*>(tlsData), tlsDataSize);
        return false;
    }
    GuestThread t{};
    t.kthread = kthread;
    t.tlsData = tlsData;
    FillKThread(kthread, guest::Addr(tlsData));
    t.tlsAnchor = guest::Addr(tlsData) + tlsDataSize;
    t.threadId = kthread->ThreadId;
    SetSlots(t, prcb);
    t_thread = t;
    t_allocation = nullptr;
    if (out != nullptr) {
        *out = t;
    }
    FUT_LOGD(Core, "adopted thread %lu (caller memory): KTHREAD %p TlsData %p anchor 0x%08X", t.threadId,
             static_cast<void*>(kthread), static_cast<void*>(tlsData), t.tlsAnchor);
    return true;
}

void ReleaseCurrentThread() noexcept {
    for (int s = 0; s < FUT_TLS_SLOT_COUNT; ++s) {
        SetGuestSlot(static_cast<FutTlsSlot>(s), 0);
    }
    if (t_allocation != nullptr) {
        VirtualFree(t_allocation, 0, MEM_RELEASE);
        t_allocation = nullptr;
    }
    t_thread = GuestThread{};
}

const GuestThread* CurrentGuestThread() noexcept {
    return t_thread.kthread != nullptr ? &t_thread : nullptr;
}

}  // namespace core
