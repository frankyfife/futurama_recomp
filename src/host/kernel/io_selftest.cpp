// Self-test of the guest I/O APC path through the original XAPI code (io agent, M2; design 1.7,
// self-tests design 1.4 step 8). Runs with --selftest in phase AdoptedThread (boot thread, before
// entry, after InitVfs).
//
// GuestReadFileExApc: the game reads assets only with XAPI ReadFileEx (NiUDSStream FUN_0013F660) and
// writes saves with WriteFileEx (FUN_000E0B00); the completions run in SleepEx(0, TRUE) (0x13F4C0,
// 0x13F590, 0xE0030). The test drives exactly that chain with the guest functions:
//   guest WriteFileEx 0xF7654 / ReadFileEx 0xF7603 -> thunk slot 35/34 NtWriteFile/NtReadFile with
//   ApcRoutine = [slot 39] NtUserIoApcDispatcher -> STATUS_PENDING, APC queued, OVERLAPPED untouched;
//   guest SleepEx 0xF64BE (0, TRUE) -> KeDelayExecutionThread(UserMode, TRUE) -> completions called
//   (dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped) in FIFO order -> SleepEx returns 0xC0.
// A read of 0x1000 bytes from the 0x800-byte file runs past the end and must succeed clipped (A8,
// files.md 2.4). The file lives on partition1 (always present) and is deleted on close. Only success
// paths are used: XAPI's error path (XapiSetLastNTError) would need the TLS index entry sets later.
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/selftest.h"
#include "kernel/io_apc.h"
#include "kernel/io_exports.h"
#include "kernel/kernel_api.h"
#include "kernel/ob_handles.h"
#include "vfs/vfs.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace krnl::io {
namespace {

constexpr std::uint32_t kFileBytes = 0x800;
constexpr std::uint32_t kReadBytes = 0x1000;       // runs 0x800 bytes past the end of the file
constexpr std::uint32_t kWaitIoCompletion = 0xC0;  // WAIT_IO_COMPLETION = STATUS_USER_APC
constexpr std::uint32_t kGenericReadWrite = 0xC0000000;
constexpr std::uint32_t kDelete = 0x00010000;
constexpr std::uint32_t kSynchronize = 0x00100000;
constexpr std::uint32_t kFileOverwriteIf = 5;
constexpr std::uint32_t kNonDirectoryDeleteOnClose = 0x00000040 | 0x00001000;  // no FILE_SYNCHRONOUS_IO_*
constexpr char kTestFile[] = "\\Device\\Harddisk0\\partition1\\fut_selftest_apc.tmp";

struct Completion {
    std::uint32_t error;
    std::uint32_t bytes;
    std::uint32_t overlapped;
};
Completion g_completions[4];
int g_completionCount = 0;

// LPOVERLAPPED_COMPLETION_ROUTINE (stdcall, ret 0xC) like the game's 0x13F1D0 / 0xE01A0.
void __stdcall SelfTestCompletion(std::uint32_t error, std::uint32_t bytes, void* overlapped) noexcept {
    if (g_completionCount < 4) {
        g_completions[g_completionCount] = Completion{error, bytes, guest::Addr(overlapped)};
    }
    ++g_completionCount;
}

// Guest-visible scratch memory below 0x80000000 (design 1.2).
struct Scratch {
    xbox::OVERLAPPED writeOvl;
    xbox::OVERLAPPED readOvl;
    std::uint8_t data[kFileBytes];
    std::uint8_t buffer[kReadBytes];
};

std::string Format(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

}  // namespace

FUT_SELFTEST(GuestReadFileExApc, AdoptedThread) {
    while (DeliverIoApcs() != 0) {  // start from an empty queue
    }
    void* page = VirtualAlloc(nullptr, sizeof(Scratch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (page == nullptr || guest::Addr(page) >= guest::kGuestMemoryLimit) {
        detail = "no scratch memory below 0x80000000";
        if (page != nullptr) {
            VirtualFree(page, 0, MEM_RELEASE);
        }
        return false;
    }
    auto* s = static_cast<Scratch*>(page);
    for (std::uint32_t i = 0; i < kFileBytes; ++i) {
        s->data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    std::memset(s->buffer, 0xEE, sizeof(s->buffer));

    // asynchronous handle (no FILE_SYNCHRONOUS_IO_*), like CreateFileA with FILE_FLAG_OVERLAPPED
    xbox::ANSI_STRING name{};
    char nameText[sizeof(kTestFile)];
    std::memcpy(nameText, kTestFile, sizeof(kTestFile));
    name.Length = static_cast<std::uint16_t>(sizeof(kTestFile) - 1);
    name.MaximumLength = static_cast<std::uint16_t>(sizeof(kTestFile));
    name.Buffer = nameText;
    xbox::OBJECT_ATTRIBUTES oa{};
    oa.RootDirectory = nullptr;
    oa.ObjectName = &name;
    oa.Attributes = xbox::OBJ_CASE_INSENSITIVE;
    xbox::IO_STATUS_BLOCK iosb{};
    HANDLE h = nullptr;
    const NTSTATUS open = NtCreateFile(&h, kGenericReadWrite | kDelete | kSynchronize, &oa, &iosb, nullptr,
                                       0x80, 0, kFileOverwriteIf, kNonDirectoryDeleteOnClose);
    if (!NT_SUCCESS(open)) {
        detail = Format("NtCreateFile(%s) failed: 0x%08lX", kTestFile, static_cast<unsigned long>(open));
        VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }

    g_completionCount = 0;
    const IoApcStats before = GetIoApcStats();
    const auto handle = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(h));
    const auto completion = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&SelfTestCompletion));
    // BOOL WriteFileEx(h, buf, len, lpOverlapped (Offset 0), completion) / ReadFileEx(...)
    const std::uint32_t wrote =
        core::CallGuest(guest::kXapiWriteFileEx,
                        {handle, guest::Addr(s->data), kFileBytes, guest::Addr(&s->writeOvl), completion});
    const std::uint32_t read =
        core::CallGuest(guest::kXapiReadFileEx,
                        {handle, guest::Addr(s->buffer), kReadBytes, guest::Addr(&s->readOvl), completion});
    const std::uint32_t pending = PendingIoApcCount();
    const bool untouched =
        s->writeOvl.Internal == 0 && s->readOvl.InternalHigh == 0 && g_completionCount == 0;
    // SleepEx(0, TRUE): the only place the game's completions run
    const std::uint32_t sleep1 = core::CallGuest(guest::kXapiSleepEx, {0, TRUE});
    const int afterFirstSleep = g_completionCount;
    const std::uint32_t sleep2 = core::CallGuest(guest::kXapiSleepEx, {0, TRUE});  // queue empty now
    const IoApcStats after = GetIoApcStats();
    NtClose(h);  // FILE_DELETE_ON_CLOSE removes the file

    const bool dataOk = std::memcmp(s->buffer, s->data, kFileBytes) == 0 && s->buffer[kFileBytes] == 0xEE;
    const bool argsOk =
        afterFirstSleep == 2 && g_completions[0].error == 0 && g_completions[0].bytes == kFileBytes &&
        g_completions[0].overlapped == guest::Addr(&s->writeOvl) && g_completions[1].error == 0 &&
        g_completions[1].bytes == kFileBytes && g_completions[1].overlapped == guest::Addr(&s->readOvl);
    const bool ovlOk = s->writeOvl.Internal == 0 && s->writeOvl.InternalHigh == kFileBytes &&
                       s->readOvl.Internal == 0 && s->readOvl.InternalHigh == kFileBytes;
    const bool statsOk = after.queued - before.queued == 2 && after.delivered - before.delivered == 2 &&
                         after.failedRequests == before.failedRequests;
    const bool ok = wrote != 0 && read != 0 && pending == 2 && untouched && sleep1 == kWaitIoCompletion &&
                    sleep2 == 0 && argsOk && ovlOk && dataOk && statsOk && PendingIoApcCount() == 0;
    detail = Format("WriteFileEx %u, ReadFileEx %u, pending %u (overlapped untouched %s), SleepEx(0,TRUE) "
                    "0x%X then 0x%X, completions %d (err %u/%u, bytes 0x%X/0x%X), read clipped at EOF %s",
                    wrote, read, pending, untouched ? "yes" : "NO", sleep1, sleep2, g_completionCount,
                    g_completions[0].error, g_completions[1].error, g_completions[0].bytes,
                    g_completions[1].bytes, dataOk ? "yes" : "NO");
    VirtualFree(page, 0, MEM_RELEASE);
    return ok;
}

}  // namespace krnl::io
