// Guest handle table and NtClose. See ob_handles.h. Part of the shared ob_handles unit.
#include "kernel/ob_handles.h"

#include "core/guest.h"
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "shared/nt_status.h"

namespace krnl {

HandleTable& HandleTable::Instance() noexcept {
    static HandleTable* table = new HandleTable();  // never destroyed: guest handles live until exit
    return *table;
}

bool HandleTable::IsPseudoHandle(HANDLE handle) noexcept {
    const auto v = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(handle));
    return v == -1 || v == -2 || v == -3;
}

HANDLE HandleTable::Insert(const Ref<KObject>& object) noexcept {
    if (!object) {
        return nullptr;
    }
    AcquireSRWLockExclusive(&lock_);
    std::size_t index = 0;
    while (index < slots_.size() && slots_[index] != nullptr) {
        ++index;
    }
    HANDLE result = nullptr;
    try {
        if (index == slots_.size() && slots_.size() < kMaxHandles) {
            slots_.push_back(nullptr);
        }
        if (index < slots_.size()) {
            object->AddRef();
            slots_[index] = object.get();
            result = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(kHandleBase + kHandleStep * index));
        }
    } catch (...) {
        result = nullptr;
    }
    ReleaseSRWLockExclusive(&lock_);
    if (result == nullptr) {
        FUT_LOGE(Krnl, "handle table full (%u handles)", kMaxHandles);
    }
    return result;
}

Ref<KObject> HandleTable::Lookup(HANDLE handle) const noexcept {
    const auto v = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle));
    if (v < kHandleBase || (v - kHandleBase) % kHandleStep != 0) {
        return {};
    }
    const std::size_t index = (v - kHandleBase) / kHandleStep;
    AcquireSRWLockShared(&lock_);
    Ref<KObject> result;
    if (index < slots_.size() && slots_[index] != nullptr) {
        result = Ref<KObject>(slots_[index]);
    }
    ReleaseSRWLockShared(&lock_);
    return result;
}

NTSTATUS HandleTable::Close(HANDLE handle) noexcept {
    const auto v = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle));
    if (v < kHandleBase || (v - kHandleBase) % kHandleStep != 0) {
        return STATUS_INVALID_HANDLE;
    }
    const std::size_t index = (v - kHandleBase) / kHandleStep;
    KObject* object = nullptr;
    AcquireSRWLockExclusive(&lock_);
    if (index < slots_.size()) {
        object = std::exchange(slots_[index], nullptr);
    }
    ReleaseSRWLockExclusive(&lock_);
    if (object == nullptr) {
        return STATUS_INVALID_HANDLE;
    }
    const NTSTATUS status = object->OnHandleClose();
    object->Release();
    return status;
}

std::size_t HandleTable::Count() const noexcept {
    AcquireSRWLockShared(&lock_);
    std::size_t n = 0;
    for (KObject* o : slots_) {
        n += o != nullptr ? 1 : 0;
    }
    ReleaseSRWLockShared(&lock_);
    return n;
}

void HandleTable::LogOpenHandles() const noexcept {
    AcquireSRWLockShared(&lock_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i] != nullptr) {
            FUT_LOGI(Krnl, "open handle 0x%04X: %s (refs %ld)", static_cast<unsigned>(kHandleBase + kHandleStep * i),
                     slots_[i]->TypeName(), slots_[i]->RefCount());
        }
    }
    ReleaseSRWLockShared(&lock_);
}

// NtClose - slot 26, Ord 187, stdcall, 1 arg (purge 4).
// Evidence: kernel_imports.tsv slot 26 (callers: XAPI CloseHandle 0xF7177 and file/thread paths);
// design 4.2 item 6. The entry thread's CloseHandle failure path reaches SetLastError (cpu.md 2.2).
NTSTATUS __stdcall NtClose(HANDLE Handle) noexcept {
    if (HandleTable::IsPseudoHandle(Handle)) {
        // ASSUMPTION: closing a pseudo handle is a harmless no-op (never observed on the boot path).
        FUT_LOGD(Krnl, "NtClose(pseudo handle %p) -> STATUS_SUCCESS", Handle);
        return STATUS_SUCCESS;
    }
    const NTSTATUS status = HandleTable::Instance().Close(Handle);
    if (core::LogEnabled(core::Channel::Krnl, core::Level::Trace) || !NT_SUCCESS(status)) {
        const char* name = xbox::NtStatusName(status);
        core::LogWrite(core::Channel::Krnl, NT_SUCCESS(status) ? core::Level::Trace : core::Level::Warn,
                       "NtClose(%p) -> 0x%08lX %s", Handle, static_cast<unsigned long>(status), name ? name : "");
    }
    return status;
}
KRNL_FUNC(NtClose, &NtClose);

}  // namespace krnl
