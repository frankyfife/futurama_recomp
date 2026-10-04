// Per-thread guest state: the five TLS slots read by the patched fs: sites (design 1.6, cpu.md 2.4 A).
// Owned by the core agent. The kernel agent calls AdoptCurrentThread() (or AdoptCurrentThreadWith()
// with its own memory) at the start of every thread created by PsCreateSystemThreadEx, before calling
// SystemRoutine; the core boot stage calls it for the boot thread that runs entry.
#pragma once

#include "shared/launcher_info.h"
#include "shared/xbox_types.h"

#include <cstdint>

namespace core {

struct GuestThread {
    xbox::FakeKThread* kthread;  // value of S_KTHREAD
    std::uint8_t* tlsData;       // KTHREAD.TlsData (nullptr on the boot thread)
    std::uint32_t tlsAnchor;     // value of S_TLS
    std::uint32_t threadId;      // KTHREAD.ThreadId (= host thread id)
};

// Prepares the CALLING thread to run guest code (design 1.6 table "Werte pro Gast-Thread"):
//  - tlsDataSize > 0 (threads of PsCreateSystemThreadEx, 0x24 for this XBE): zeroed TlsData of that
//    size; KTHREAD.TlsData = TlsData; S_TLS = TlsData + tlsDataSize. XapiThreadStartup then writes
//    [TlsData] = TlsData + 4 itself.
//  - tlsDataSize == 0 (boot thread running entry): KTHREAD.TlsData = 0 and S_TLS points to a valid
//    dummy TLS area whose block pointer is set (CloseHandle failure path -> SetLastError, cpu.md 2.2).
//  - fake KTHREAD (0x140 bytes, zeroed) with StackBase (TEB.StackBase), StackLimit (lowest address of
//    the thread's stack reservation) and ThreadId (GetCurrentThreadId).
//  - S_PRCB = shared zeroed fake KPRCB (+0x250 = 0), S_IRQL = 0, S_DPC = 0.
// Memory comes from VirtualAlloc below 0x80000000 and is freed by ReleaseCurrentThread. Also names
// the thread for the log.
bool AdoptCurrentThread(std::uint32_t tlsDataSize, const char* threadName,
                        GuestThread* out = nullptr) noexcept;

// Same for callers that own the memory (e.g. the kernel's thread object): fills `kthread`
// (StackBase/StackLimit/ThreadId as above, TlsData = tlsData) and sets the 5 TLS slots
// (S_TLS = tlsData + tlsDataSize). `kthread` must be a zeroed FakeKThread and `tlsData` tlsDataSize
// (>= 4) zeroed bytes, both below 0x80000000 and alive until the thread ends. Nothing is allocated;
// ReleaseCurrentThread then only clears the slots.
bool AdoptCurrentThreadWith(xbox::FakeKThread* kthread, std::uint8_t* tlsData, std::uint32_t tlsDataSize,
                            const char* threadName, GuestThread* out = nullptr) noexcept;

// Zeroes the TLS slots of the calling thread and frees what AdoptCurrentThread allocated.
void ReleaseCurrentThread() noexcept;
// State of the calling thread (nullptr if it was not adopted).
const GuestThread* CurrentGuestThread() noexcept;

xbox::FakeKPrcb* SharedFakePrcb() noexcept;

// Raw TLS slot access for the calling thread (TlsGetValue/TlsSetValue on the launcher's index).
std::uint32_t GetGuestSlot(FutTlsSlot slot) noexcept;
void SetGuestSlot(FutTlsSlot slot, std::uint32_t value) noexcept;

}  // namespace core
