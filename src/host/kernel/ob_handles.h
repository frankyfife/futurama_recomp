// Guest handle table (design 4.2 item 6): guest HANDLE values -> refcounted host objects.
//
// Guest handles are small values 0x1000 + 4n (never -1/-2/-3, which are the pseudo handles
// NtCurrentProcess / NtCurrentThread / ObDosDevicesDirectory). Every kernel object type (files,
// directory enumerations, threads, symbolic links, devices, ...) derives from krnl::KObject and is
// inserted with HandleTable::Insert. NtClose (slot 26) is implemented in ob_handles.cpp for all types.
//
//   class FileObject final : public krnl::KObject {
//   public:
//       const char* TypeName() const noexcept override { return "File"; }
//       NTSTATUS OnHandleClose() noexcept override { /* per-handle cleanup */ return STATUS_SUCCESS; }
//       ~FileObject() override { /* release host resources when the last reference goes */ }
//   };
//   auto file = krnl::MakeObject<FileObject>(...);
//   HANDLE h = krnl::HandleTable::Instance().Insert(file);         // table holds its own reference
//   if (auto f = krnl::HandleTable::Instance().LookupAs<FileObject>(h)) { ... }
#pragma once

#include "shared/xbox_types.h"

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace krnl {

class KObject {
public:
    KObject() noexcept = default;
    KObject(const KObject&) = delete;
    KObject& operator=(const KObject&) = delete;
    virtual ~KObject() = default;

    // Short type name for logs ("File", "Thread", "SymbolicLink", ...).
    virtual const char* TypeName() const noexcept = 0;
    // Called when a guest handle to this object is closed, before the table drops its reference.
    // The return value becomes the NtClose status.
    virtual NTSTATUS OnHandleClose() noexcept { return STATUS_SUCCESS; }

    void AddRef() noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }
    void Release() noexcept {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
    long RefCount() const noexcept { return refs_.load(std::memory_order_relaxed); }

private:
    std::atomic<long> refs_{1};  // the creator's reference (adopted by Ref<T>::Adopt / MakeObject)
};

// Intrusive reference to a KObject (or derived type).
template <class T>
class Ref {
public:
    Ref() noexcept = default;
    Ref(std::nullptr_t) noexcept {}
    // Takes an additional reference.
    explicit Ref(T* p) noexcept : p_(p) {
        if (p_) p_->AddRef();
    }
    // Adopts an existing reference (no AddRef), e.g. the initial one of a new object.
    static Ref Adopt(T* p) noexcept {
        Ref r;
        r.p_ = p;
        return r;
    }
    Ref(const Ref& o) noexcept : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    Ref(Ref&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    template <class U>
    Ref(const Ref<U>& o) noexcept : p_(o.get()) {
        if (p_) p_->AddRef();
    }
    template <class U>
    Ref(Ref<U>&& o) noexcept : p_(o.release()) {}
    ~Ref() {
        if (p_) p_->Release();
    }
    Ref& operator=(Ref o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    // Gives up ownership without releasing.
    T* release() noexcept { return std::exchange(p_, nullptr); }

private:
    T* p_ = nullptr;
};

template <class T, class... Args>
Ref<T> MakeObject(Args&&... args) {
    return Ref<T>::Adopt(new T(std::forward<Args>(args)...));
}

inline constexpr std::uint32_t kHandleBase = 0x1000;
inline constexpr std::uint32_t kHandleStep = 4;
inline constexpr std::uint32_t kMaxHandles = 0x10000;

class HandleTable {
public:
    static HandleTable& Instance() noexcept;

    // Inserts a new reference to `object`; returns the guest handle, or nullptr if the table is full
    // or object is null. The lowest free handle value is reused (deterministic, like NT).
    HANDLE Insert(const Ref<KObject>& object) noexcept;
    // Reference to the object behind `handle` (null Ref for unknown/pseudo handles).
    Ref<KObject> Lookup(HANDLE handle) const noexcept;
    template <class T>
    Ref<T> LookupAs(HANDLE handle) const noexcept {
        Ref<KObject> o = Lookup(handle);
        if (T* t = dynamic_cast<T*>(o.get())) {
            return Ref<T>(t);
        }
        return {};
    }
    // Removes the handle: calls OnHandleClose(), drops the table's reference. STATUS_INVALID_HANDLE
    // for unknown handles.
    NTSTATUS Close(HANDLE handle) noexcept;
    std::size_t Count() const noexcept;
    // Logs every open handle (debug aid, CORE/info).
    void LogOpenHandles() const noexcept;

    static bool IsPseudoHandle(HANDLE handle) noexcept;

private:
    HandleTable() = default;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<KObject*> slots_;  // index n <-> handle kHandleBase + 4n; nullptr = free
};

// NtClose - slot 26, Ord 187 (0x0BB), stdcall, 1 arg (purge 4).
// Evidence: kernel_imports.tsv slot 26; kernel.md 7 group 3; design 4.2 item 6 / 4.3.
NTSTATUS __stdcall NtClose(HANDLE Handle) noexcept;

}  // namespace krnl
