// Guest-visible Xbox kernel structures used only inside the kernel HLE (design 1.7, 5.3).
// All structs are POD with the Xbox layout (x86, 32-bit pointers = host pointers).
// Owned by the kernel agent. Structures shared with other modules live in shared/xbox_types.h.
#pragma once

#include "shared/xbox_types.h"

#include <cstddef>
#include <cstdint>

namespace krnl::guestdef {

// LIST_ENTRY (8 bytes).
struct LIST_ENTRY {
    LIST_ENTRY* Flink;
    LIST_ENTRY* Blink;
};
static_assert(sizeof(LIST_ENTRY) == 8);

// DISPATCHER_HEADER: first member of KEVENT, KTIMER, ... (0x10 bytes, NT layout).
// ASSUMPTION: field layout after the XDK/NT 5.0 headers; the guest initializes KEVENTs inline
// (e.g. the IDE-shutdown event 0x2D7540 used by sub_0f9c63/sub_0f9c7f, kernel.md 8.7). The HLE
// only reads Type and SignalState and mirrors SignalState (design 1.7: state lives in a host map).
struct DISPATCHER_HEADER {
    std::uint8_t Type;         // +0x00 0 = NotificationEvent, 1 = SynchronizationEvent, 8/9 = timer
    std::uint8_t Absolute;     // +0x01
    std::uint8_t Size;         // +0x02 in DWORDs
    std::uint8_t Inserted;     // +0x03
    std::int32_t SignalState;  // +0x04
    LIST_ENTRY WaitListHead;   // +0x08
};
static_assert(sizeof(DISPATCHER_HEADER) == 0x10);
static_assert(offsetof(DISPATCHER_HEADER, SignalState) == 0x04);
static_assert(offsetof(DISPATCHER_HEADER, WaitListHead) == 0x08);

inline constexpr std::uint8_t kEventNotificationObject = 0;
inline constexpr std::uint8_t kEventSynchronizationObject = 1;
inline constexpr std::uint8_t kTimerNotificationObject = 8;
inline constexpr std::uint8_t kTimerSynchronizationObject = 9;

// RTL_CRITICAL_SECTION (Xbox): embedded event instead of the Win32 debug-info layout.
// Size 0x1C is evidenced by the guest: CRT _mtinitlocknum mallocs 0x1C bytes per lock
// (FUN_0016ebfd, _text_00160000.c) and the CRT lowio entries (stride 0x28) embed the lock at +0xC
// (FUN_0016eca7). ASSUMPTION: field order {Event, LockCount, RecursionCount, OwningThread} after
// the XDK headers. The HLE keeps the real lock in a host CRITICAL_SECTION (design 1.7) and only
// mirrors these fields for guest code that might read them (none found, design risk R5).
struct RTL_CRITICAL_SECTION {
    DISPATCHER_HEADER Event;      // +0x00
    std::int32_t LockCount;       // +0x10 -1 = free
    std::int32_t RecursionCount;  // +0x14
    std::uint32_t OwningThread;   // +0x18 KTHREAD* of the owner (fake KTHREAD, S_KTHREAD)
};
static_assert(sizeof(RTL_CRITICAL_SECTION) == 0x1C);
static_assert(offsetof(RTL_CRITICAL_SECTION, LockCount) == 0x10);
static_assert(offsetof(RTL_CRITICAL_SECTION, RecursionCount) == 0x14);
static_assert(offsetof(RTL_CRITICAL_SECTION, OwningThread) == 0x18);

// MEMORY_BASIC_INFORMATION as returned by the Xbox NtQueryVirtualMemory (same as NT x86).
// Evidence: XAPI heap FUN_000f4ea8 reads +0x00 BaseAddress, +0x0C RegionSize and +0x10 State
// (compares with MEM_FREE 0x10000 / MEM_COMMIT 0x1000 / MEM_RESERVE 0x2000), _text_000f0000.c.
struct MEMORY_BASIC_INFORMATION {
    std::uint32_t BaseAddress;        // +0x00
    std::uint32_t AllocationBase;     // +0x04
    std::uint32_t AllocationProtect;  // +0x08
    std::uint32_t RegionSize;         // +0x0C
    std::uint32_t State;              // +0x10
    std::uint32_t Protect;            // +0x14
    std::uint32_t Type;               // +0x18
};
static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 0x1C);
static_assert(offsetof(MEMORY_BASIC_INFORMATION, RegionSize) == 0x0C);
static_assert(offsetof(MEMORY_BASIC_INFORMATION, State) == 0x10);
static_assert(sizeof(MEMORY_BASIC_INFORMATION) == sizeof(::MEMORY_BASIC_INFORMATION));

// XBOX_HARDWARE_INFO (slot 64 DATA). Byte +0 = Flags (bits 0x1/0x2/0x8 tested by XAPI/XPP),
// +1 GPU revision bit 0x2 (D3D 0x23C84C), +5 compared with 0xA1 (XPP 0x28C8FB); kernel.md 5.
struct XBOX_HARDWARE_INFO {
    std::uint32_t Flags;       // +0x00 bit 3 MUST be 0 (files.md 5.5.7, design 1.5)
    std::uint8_t GpuRevision;  // +0x04
    std::uint8_t McpRevision;  // +0x05
    std::uint8_t Reserved[2];  // +0x06
};
static_assert(sizeof(XBOX_HARDWARE_INFO) == 8);
static_assert(offsetof(XBOX_HARDWARE_INFO, McpRevision) == 5);

// XBOX_KRNL_VERSION (slot 75 DATA): {Major, Minor, Build, Qfe}; FUN_000f9804 reads Build at +4.
struct XBOX_KRNL_VERSION {
    std::uint16_t Major;
    std::uint16_t Minor;
    std::uint16_t Build;
    std::uint16_t Qfe;
};
static_assert(sizeof(XBOX_KRNL_VERSION) == 8);
static_assert(offsetof(XBOX_KRNL_VERSION, Build) == 4);

// Registry value types returned by ExQueryNonVolatileSetting.
inline constexpr std::uint32_t kRegBinary = 3;
inline constexpr std::uint32_t kRegDword = 4;

}  // namespace krnl::guestdef
