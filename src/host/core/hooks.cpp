// Hook registry and installer (design 3.2). Owned by the core agent; skeleton version with the
// validation rules of design 3.2 implemented.
#include "core/hooks.h"

#include "core/log.h"
#include "core/xbe_loader.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace core {
namespace {

std::vector<HookDesc>& Registry() {
    static auto* hooks = new std::vector<HookDesc>();  // filled by static initializers
    return *hooks;
}

std::vector<InstalledHook>& InstalledHooks() {
    static auto* installed = new std::vector<InstalledHook>();
    return *installed;
}

constexpr std::uint32_t kJmpSize = 5;
constexpr std::uint32_t kMaxTrampolineBytes = 16;
constexpr std::uint32_t kTrampolineSlotSize = 32;  // stolen bytes + E9 rel32, padded with int3
constexpr std::uint32_t kTrampolinePageSize = 0x1000;

// Bytes a hook occupies in the image for overlap checks: the 5-byte jmp, or all stolen bytes of a
// probe (they must stay intact because the trampoline mirrors them).
std::uint32_t PatchLength(std::uint32_t trampolineBytes) {
    return trampolineBytes > kJmpSize ? trampolineBytes : kJmpSize;
}

// Executable memory for probe trampolines (host memory, never handed to the guest as data). Hooks
// are installed on the boot thread only (StageInstallHooks, InstallHookNow before entry).
std::uint8_t* AllocTrampolineSlot() {
    static std::uint8_t* page = nullptr;
    static std::uint32_t used = kTrampolinePageSize;
    if (used + kTrampolineSlotSize > kTrampolinePageSize) {
        page = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, kTrampolinePageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (page == nullptr) {
            return nullptr;
        }
        std::memset(page, 0xCC, kTrampolinePageSize);
        used = 0;
    }
    std::uint8_t* slot = page + used;
    used += kTrampolineSlotSize;
    return slot;
}

bool ParseHex(const char* text, std::vector<std::uint8_t>& out) {
    out.clear();
    int nibbles = 0;
    std::uint8_t cur = 0;
    for (const char* p = text; *p; ++p) {
        int v;
        if (*p >= '0' && *p <= '9')
            v = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            v = *p - 'A' + 10;
        else if (*p == ' ')
            continue;
        else
            return false;
        cur = static_cast<std::uint8_t>((cur << 4) | v);
        if (++nibbles == 2) {
            out.push_back(cur);
            nibbles = 0;
            cur = 0;
        }
    }
    return nibbles == 0;
}

bool Overlaps(guest::GuestAddr a, std::uint32_t aLen, guest::GuestAddr b, std::uint32_t bLen) {
    return a < b + bLen && b < a + aLen;
}

bool Validate(const HookDesc& d, std::string& why) {
    char buf[256];
    const guest::FunctionInfo* f = guest::FindFunctionStart(d.address);
    if (f == nullptr) {
        why = "address is not a known function start (gen/xbe_functions.gen.inc)";
        return false;
    }
    const std::uint32_t patchLength = PatchLength(d.trampolineBytes);
    if (f->size < patchLength) {
        std::snprintf(buf, sizeof(buf), "function %s is only %u bytes (< %u)", f->name, f->size, patchLength);
        why = buf;
        return false;
    }
    if (d.replacement == nullptr) {
        why = "null replacement";
        return false;
    }
    if (d.trampolineBytes != 0 && (d.trampolineBytes < kJmpSize || d.trampolineBytes > kMaxTrampolineBytes ||
                                   d.expectedBytes == nullptr)) {
        why = "probe hook needs 5..16 stolen bytes listed in expectedBytes";
        return false;
    }
    for (const auto& h : InstalledHooks()) {
        if (Overlaps(h.address, PatchLength(h.trampolineBytes), d.address, patchLength)) {
            std::snprintf(buf, sizeof(buf), "overlaps installed hook %s at 0x%08X", h.name, h.address);
            why = buf;
            return false;
        }
    }
    for (const auto& s : guest::FsPatchSites()) {
        if (Overlaps(s.address, s.length, d.address, patchLength)) {
            std::snprintf(buf, sizeof(buf), "overlaps fs patch site 0x%08X", s.address);
            why = buf;
            return false;
        }
    }
    // Design 1.4 step 6 / 3.2: the target must still hold the original XBE bytes (no thunk, fs or
    // other patch underneath, correct XBE build). Probes: all stolen bytes.
    std::uint8_t original[kMaxTrampolineBytes];
    if (!ReadOriginalXbeBytes(d.address, original, patchLength)) {
        why = "XBE not loaded/mapped or address outside the image";
        return false;
    }
    if (std::memcmp(guest::Ptr<const void>(d.address), original, patchLength) != 0) {
        why = "bytes at the target differ from the XBE file (already patched?)";
        return false;
    }
    if (d.expectedBytes != nullptr) {
        std::vector<std::uint8_t> expected;
        if (!ParseHex(d.expectedBytes, expected)) {
            why = "malformed expectedBytes";
            return false;
        }
        if (d.trampolineBytes != 0 && expected.size() != d.trampolineBytes) {
            std::snprintf(buf, sizeof(buf), "probe lists %zu prologue bytes but steals %u", expected.size(),
                          d.trampolineBytes);
            why = buf;
            return false;
        }
        if (std::memcmp(guest::Ptr<const void>(d.address), expected.data(), expected.size()) != 0) {
            why = "original bytes differ from expectedBytes";
            return false;
        }
    }
    return true;
}

// Probe trampoline: copy of the stolen original bytes, then `jmp address+trampolineBytes`.
guest::GuestAddr BuildTrampoline(const HookDesc& d) {
    std::uint8_t* slot = AllocTrampolineSlot();
    if (slot == nullptr) {
        return 0;
    }
    std::memcpy(slot, guest::Ptr<const void>(d.address), d.trampolineBytes);
    std::uint8_t* jmp = slot + d.trampolineBytes;
    const std::uint32_t back = d.address + d.trampolineBytes;
    const std::uint32_t rel = back - (guest::Addr(jmp) + kJmpSize);
    jmp[0] = 0xE9;
    std::memcpy(jmp + 1, &rel, sizeof(rel));
    FlushInstructionCache(GetCurrentProcess(), slot, kTrampolineSlotSize);
    return guest::Addr(slot);
}

bool Install(const HookDesc& d) {
    std::string why;
    if (!Validate(d, why)) {
        FUT_LOGE(Hook, "hook %s at 0x%08X rejected: %s (%s)", d.name, d.address, why.c_str(), d.file);
        return false;
    }
    guest::GuestAddr trampoline = 0;
    if (d.trampolineBytes != 0) {
        trampoline = BuildTrampoline(d);
        if (trampoline == 0) {
            FUT_LOGE(Hook, "hook %s at 0x%08X: cannot allocate a trampoline", d.name, d.address);
            return false;
        }
    }
    std::uint8_t jmp[kJmpSize];
    jmp[0] = 0xE9;
    const std::uint32_t rel =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(d.replacement)) - (d.address + kJmpSize);
    std::memcpy(&jmp[1], &rel, sizeof(rel));
    if (!guest::WriteBytes(d.address, jmp, kJmpSize)) {
        FUT_LOGE(Hook, "hook %s at 0x%08X: cannot write the jump", d.name, d.address);
        return false;
    }
    InstalledHooks().push_back({d.address, d.replacement, d.name, trampoline, d.trampolineBytes});
    if (trampoline != 0) {
        FUT_LOGI(Hook, "hook %-40s %s -> %p (probe, %u bytes in trampoline)", d.name,
                 guest::Symbolize(d.address).c_str(), d.replacement, d.trampolineBytes);
    } else {
        FUT_LOGI(Hook, "hook %-40s %s -> %p", d.name, guest::Symbolize(d.address).c_str(), d.replacement);
    }
    return true;
}

}  // namespace

void RegisterHook(const HookDesc& desc) noexcept {
    try {
        Registry().push_back(desc);
    } catch (...) {
    }
}

bool IsHookDisabled(const HookDesc& desc, const Config& config) {
    for (const std::string& entry : config.hooksDisable) {
        if (entry == desc.name) {
            return true;
        }
        if (!entry.empty() && entry.back() == '*' &&
            std::string_view(desc.name).substr(0, entry.size() - 1) ==
                std::string_view(entry).substr(0, entry.size() - 1)) {
            return true;
        }
        char addr[16];
        std::snprintf(addr, sizeof(addr), "0x%X", desc.address);
        if (_stricmp(entry.c_str(), addr) == 0) {
            return true;
        }
    }
    return false;
}

bool StageInstallHooks(const Config& config) {
    std::vector<HookDesc> hooks = Registry();
    std::sort(hooks.begin(), hooks.end(),
              [](const HookDesc& a, const HookDesc& b) { return a.address < b.address; });
    bool ok = true;
    std::size_t installed = 0, disabled = 0;
    for (const HookDesc& d : hooks) {
        if (!d.defaultOn || IsHookDisabled(d, config)) {
            ++disabled;
            FUT_LOGI(Hook, "hook %s at 0x%08X disabled", d.name, d.address);
            continue;
        }
        if (Install(d)) {
            ++installed;
        } else {
            ok = false;
        }
    }
    FUT_LOGI(Hook, "hooks: %zu installed, %zu disabled, %zu registered", installed, disabled, hooks.size());
    return ok;
}

bool InstallHookNow(const HookDesc& desc) noexcept {
    try {
        return Install(desc);
    } catch (...) {
        return false;
    }
}

std::vector<InstalledHook> InstalledHookList() {
    return InstalledHooks();
}

guest::GuestAddr HookTrampoline(guest::GuestAddr address) noexcept {
    for (const auto& h : InstalledHooks()) {
        if (h.address == address) {
            return h.trampoline;
        }
    }
    return 0;
}

bool IsHookInstalled(guest::GuestAddr address) noexcept {
    for (const auto& h : InstalledHooks()) {
        if (h.address == address) {
            return true;
        }
    }
    return false;
}

}  // namespace core
