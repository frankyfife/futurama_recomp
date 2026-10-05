// Guest-visible host memory below 0x80000000 (see guest_memory.h).
#include "gfx/guest_memory.h"

#include "core/guest.h"
#include "core/log.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace gfx {
namespace {

constexpr std::size_t kSlabSize = 0x10000;  // VirtualAlloc granularity
constexpr std::size_t kBlocksPerSlab = kSlabSize / kGuestBlockSize;

bool BelowGuestLimit(const void* p, std::size_t size) noexcept {
    return static_cast<std::uint64_t>(guest::Addr(p)) + size <= guest::kGuestMemoryLimit;
}

struct FreeBlock {
    FreeBlock* next;
};

std::mutex g_blockMutex;
FreeBlock* g_freeBlocks = nullptr;
std::size_t g_blocksInUse = 0;

}  // namespace

void* AllocGuestPages(std::size_t size) noexcept {
    if (size == 0) {
        size = 1;
    }
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p == nullptr) {
        FUT_LOGE(Gfx, "VirtualAlloc(%zu) for guest-visible memory failed (Win32 error %lu)", size,
                 GetLastError());
        return nullptr;
    }
    if (!BelowGuestLimit(p, size)) {
        FUT_LOGE(Gfx, "guest-visible memory %p (+%zu) lies above 0x80000000 - rejected", p, size);
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    return p;  // zero-filled by VirtualAlloc
}

void FreeGuestPages(void* pages) noexcept {
    if (pages != nullptr) {
        VirtualFree(pages, 0, MEM_RELEASE);
    }
}

void* AllocGuestBlock(std::size_t size) noexcept {
    if (size > kGuestBlockSize) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_blockMutex);
    if (g_freeBlocks == nullptr) {
        auto* slab = static_cast<std::uint8_t*>(AllocGuestPages(kSlabSize));
        if (slab == nullptr) {
            return nullptr;
        }
        // Slabs are never returned to the system (the shim needs a handful of headers at a time).
        for (std::size_t i = kBlocksPerSlab; i-- > 0;) {
            auto* b = reinterpret_cast<FreeBlock*>(slab + i * kGuestBlockSize);
            b->next = g_freeBlocks;
            g_freeBlocks = b;
        }
    }
    FreeBlock* b = g_freeBlocks;
    g_freeBlocks = b->next;
    ++g_blocksInUse;
    std::memset(b, 0, kGuestBlockSize);
    return b;
}

void FreeGuestBlock(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_blockMutex);
    std::memset(block, 0xDD, kGuestBlockSize);  // stale guest pointers read garbage, not old headers
    auto* b = static_cast<FreeBlock*>(block);
    b->next = g_freeBlocks;
    g_freeBlocks = b;
    --g_blocksInUse;
}

std::size_t GuestBlocksInUse() noexcept {
    std::lock_guard<std::mutex> lock(g_blockMutex);
    return g_blocksInUse;
}

}  // namespace gfx
