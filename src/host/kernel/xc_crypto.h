// SHA-1 / HMAC-SHA1 for the Xc* crypto exports and the save-signature keys (design 4.5).
// Owned by the kernel agent.
//
// The guest owns the context buffer: XAPI passes 0x74-byte buffers to XcSHAInit/Update/Final
// (FUN_000f902f: local_8c[116]; XCalculateSignatureBeginEx 0xF706F: LocalAlloc(0x7C) with the
// context at +8, design 4.5). The SHA-1 state is kept INSIDE that buffer (ShaContext, 0x5C bytes),
// so no host state is needed and the guest may copy or reuse contexts freely (FUN_000f6f88 calls
// XcSHAFinal and then XcSHAInit on the same buffer).
#pragma once

#include <cstddef>
#include <cstdint>

namespace krnl::xc {

inline constexpr std::uint32_t kGuestShaContextSize = 0x74;  // design 4.5
inline constexpr std::uint32_t kShaDigestSize = 20;
inline constexpr std::uint32_t kShaBlockSize = 64;

// Lives in the guest's context buffer (POD).
struct ShaContext {
    std::uint32_t state[5];              // +0x00 H0..H4
    std::uint32_t countLow;              // +0x14 total bytes hashed (low)
    std::uint32_t countHigh;             // +0x18 total bytes hashed (high)
    std::uint8_t buffer[kShaBlockSize];  // +0x1C pending partial block
};
static_assert(sizeof(ShaContext) == 0x5C);
static_assert(sizeof(ShaContext) <= kGuestShaContextSize, "SHA-1 state must fit the guest buffer");

void ShaInit(ShaContext& ctx) noexcept;
void ShaUpdate(ShaContext& ctx, const void* data, std::size_t length) noexcept;
void ShaFinal(ShaContext& ctx, std::uint8_t digest[kShaDigestSize]) noexcept;
void Sha1(const void* data, std::size_t length, std::uint8_t digest[kShaDigestSize]) noexcept;

// RFC 2104 HMAC-SHA1 (keys longer than 64 bytes are hashed first).
void HmacSha1(const void* key, std::size_t keyLength, const void* data, std::size_t dataLength,
              std::uint8_t digest[kShaDigestSize]) noexcept;

}  // namespace krnl::xc
