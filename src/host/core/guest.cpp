// Guest memory helpers and generated-table access. See guest.h.
#include "core/guest.h"

#include <algorithm>
#include <cstdio>

namespace guest {
namespace {

constexpr FunctionInfo kFunctionTable[] = {
#include "gen/xbe_functions.gen.inc"
};

constexpr FsPatchSite kFsPatchTable[] = {
#include "gen/fs_patch_table.gen.inc"
};

static_assert(sizeof(kFsPatchTable) / sizeof(kFsPatchTable[0]) == 38, "design 1.6: exactly 38 fs patch sites");

}  // namespace

std::span<const FunctionInfo> Functions() noexcept { return kFunctionTable; }

std::span<const FsPatchSite> FsPatchSites() noexcept { return kFsPatchTable; }

const FunctionInfo* FindFunctionContaining(GuestAddr addr) noexcept {
    const auto table = Functions();
    auto it = std::upper_bound(table.begin(), table.end(), addr,
                               [](GuestAddr a, const FunctionInfo& f) { return a < f.address; });
    // Ghidra functions can overlap (9 cases); walk back a few entries to find one that contains addr.
    for (int back = 0; back < 4 && it != table.begin(); ++back) {
        --it;
        if (addr >= it->address && addr - it->address < std::max<std::uint32_t>(it->size, 1)) {
            return &*it;
        }
    }
    return nullptr;
}

const FunctionInfo* FindFunctionStart(GuestAddr addr) noexcept {
    const auto table = Functions();
    auto it = std::lower_bound(table.begin(), table.end(), addr,
                               [](const FunctionInfo& f, GuestAddr a) { return f.address < a; });
    if (it == table.end() || it->address != addr || it->flags == kFunctionSyntheticRange) {
        return nullptr;
    }
    return &*it;
}

std::string Symbolize(GuestAddr addr) {
    char buf[160];
    const FunctionInfo* f = InXbeImage(addr) ? FindFunctionContaining(addr) : nullptr;
    if (f == nullptr) {
        std::snprintf(buf, sizeof(buf), "0x%08X", addr);
    } else if (addr == f->address) {
        std::snprintf(buf, sizeof(buf), "0x%08X %s", addr, f->name);
    } else {
        std::snprintf(buf, sizeof(buf), "0x%08X %s+0x%X", addr, f->name, addr - f->address);
    }
    return buf;
}

bool IsReadable(GuestAddr addr, std::uint32_t size) noexcept {
    GuestAddr cur = addr;
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + size;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(Ptr<const void>(cur), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
            return false;
        }
        const DWORD p = mbi.Protect & 0xFF;
        if ((mbi.Protect & PAGE_GUARD) != 0 || p == PAGE_NOACCESS || p == PAGE_EXECUTE) {
            return false;
        }
        const std::uint64_t regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd >= end) {
            return true;
        }
        cur = static_cast<GuestAddr>(regionEnd);
    }
    return true;
}

bool WriteBytes(GuestAddr addr, const void* data, std::uint32_t size) noexcept {
    if (size == 0) {
        return true;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(Ptr<void>(addr), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    std::memcpy(Ptr<void>(addr), data, size);
    DWORD ignored = 0;
    VirtualProtect(Ptr<void>(addr), size, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), Ptr<const void>(addr), size);
    return true;
}

std::string ReadAnsiString(const xbox::ANSI_STRING* s) {
    if (s == nullptr || s->Buffer == nullptr || s->Length == 0) {
        return {};
    }
    if (!IsReadable(Addr(s->Buffer), s->Length)) {
        return {};
    }
    return std::string(s->Buffer, s->Length);
}

}  // namespace guest
