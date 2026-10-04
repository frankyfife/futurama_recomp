// SHA-1, HMAC-SHA1 and the xboxkrnl Xc* exports (slots 27/28/29/72). Owned by the kernel agent.
#include "kernel/xc_crypto.h"

#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"

#include <cstring>

namespace krnl {
namespace xc {
namespace {

inline std::uint32_t Rol(std::uint32_t v, int n) noexcept {
    return (v << n) | (v >> (32 - n));
}

void Transform(std::uint32_t state[5], const std::uint8_t block[kShaBlockSize]) noexcept {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) |
               (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) |
               static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const std::uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(b, 30);
        b = a;
        a = t;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

}  // namespace

void ShaInit(ShaContext& ctx) noexcept {
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.state[0] = 0x67452301;
    ctx.state[1] = 0xEFCDAB89;
    ctx.state[2] = 0x98BADCFE;
    ctx.state[3] = 0x10325476;
    ctx.state[4] = 0xC3D2E1F0;
}

void ShaUpdate(ShaContext& ctx, const void* data, std::size_t length) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t used = ctx.countLow % kShaBlockSize;
    const std::uint32_t oldLow = ctx.countLow;
    ctx.countLow += static_cast<std::uint32_t>(length);
    if (ctx.countLow < oldLow) {
        ++ctx.countHigh;
    }
    while (length > 0) {
        const std::size_t take = (length < kShaBlockSize - used) ? length : kShaBlockSize - used;
        std::memcpy(ctx.buffer + used, p, take);
        used += static_cast<std::uint32_t>(take);
        p += take;
        length -= take;
        if (used == kShaBlockSize) {
            Transform(ctx.state, ctx.buffer);
            used = 0;
        }
    }
}

void ShaFinal(ShaContext& ctx, std::uint8_t digest[kShaDigestSize]) noexcept {
    const std::uint64_t bits = ((static_cast<std::uint64_t>(ctx.countHigh) << 32) | ctx.countLow) * 8u;
    std::uint8_t pad[kShaBlockSize + 8] = {0x80};
    const std::uint32_t used = ctx.countLow % kShaBlockSize;
    const std::uint32_t padLen = (used < 56) ? (56 - used) : (120 - used);
    std::uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) {
        lengthBytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    }
    ShaUpdate(ctx, pad, padLen);
    ShaUpdate(ctx, lengthBytes, 8);
    for (int i = 0; i < 5; ++i) {
        digest[4 * i] = static_cast<std::uint8_t>(ctx.state[i] >> 24);
        digest[4 * i + 1] = static_cast<std::uint8_t>(ctx.state[i] >> 16);
        digest[4 * i + 2] = static_cast<std::uint8_t>(ctx.state[i] >> 8);
        digest[4 * i + 3] = static_cast<std::uint8_t>(ctx.state[i]);
    }
}

void Sha1(const void* data, std::size_t length, std::uint8_t digest[kShaDigestSize]) noexcept {
    ShaContext ctx;
    ShaInit(ctx);
    ShaUpdate(ctx, data, length);
    ShaFinal(ctx, digest);
}

void HmacSha1(const void* key, std::size_t keyLength, const void* data, std::size_t dataLength,
              std::uint8_t digest[kShaDigestSize]) noexcept {
    std::uint8_t k[kShaBlockSize] = {};
    if (keyLength > kShaBlockSize) {
        Sha1(key, keyLength, k);
    } else if (keyLength > 0) {
        std::memcpy(k, key, keyLength);
    }
    std::uint8_t ipad[kShaBlockSize], opad[kShaBlockSize];
    for (std::uint32_t i = 0; i < kShaBlockSize; ++i) {
        ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5C);
    }
    std::uint8_t inner[kShaDigestSize];
    ShaContext ctx;
    ShaInit(ctx);
    ShaUpdate(ctx, ipad, sizeof(ipad));
    ShaUpdate(ctx, data, dataLength);
    ShaFinal(ctx, inner);
    ShaInit(ctx);
    ShaUpdate(ctx, opad, sizeof(opad));
    ShaUpdate(ctx, inner, sizeof(inner));
    ShaFinal(ctx, digest);
}

}  // namespace xc

namespace {
xc::ShaContext* GuestContext(unsigned char* p, const char* fn) noexcept {
    if (p == nullptr) {
        FUT_LOGE(Krnl, "%s: NULL SHA context", fn);
    }
    return reinterpret_cast<xc::ShaContext*>(p);
}
}  // namespace

// XcSHAInit - slot 28, Ord 335, stdcall, 1 arg (purge 4): (PUCHAR pbSHAContext).
// Evidence: kernel_imports.tsv; kernel.md 7.5; callers FUN_000f6f26 (HMAC ipad), FUN_000f6f88,
// FUN_000f902f (0x74-byte stack context); design 4.5 (state inside the guest buffer).
void __stdcall XcSHAInit(unsigned char* pbSHAContext) noexcept {
    if (xc::ShaContext* ctx = GuestContext(pbSHAContext, "XcSHAInit")) {
        xc::ShaInit(*ctx);
        FUT_LOGT(Krnl, "XcSHAInit(%p)", static_cast<void*>(pbSHAContext));
    }
}
KRNL_FUNC(XcSHAInit, &XcSHAInit);

// XcSHAUpdate - slot 27, Ord 336, stdcall, 3 args (purge 12): (PUCHAR pbSHAContext, PUCHAR pbInput,
// ULONG dwInputLength). Evidence: kernel_imports.tsv; FUN_000f6f26 (0x40), FUN_000f6f88 (0x54),
// FUN_000f6ffa (0x14), XCalculateSignatureUpdate 0xF70EC (caller data), FUN_000f902f.
void __stdcall XcSHAUpdate(unsigned char* pbSHAContext, const unsigned char* pbInput,
                           ULONG dwInputLength) noexcept {
    if (xc::ShaContext* ctx = GuestContext(pbSHAContext, "XcSHAUpdate")) {
        if (pbInput == nullptr && dwInputLength != 0) {
            FUT_LOGE(Krnl, "XcSHAUpdate(%p, NULL, %lu): NULL input", static_cast<void*>(pbSHAContext),
                     dwInputLength);
            return;
        }
        xc::ShaUpdate(*ctx, pbInput, dwInputLength);
        FUT_LOGT(Krnl, "XcSHAUpdate(%p, %p, 0x%lX)", static_cast<void*>(pbSHAContext),
                 static_cast<const void*>(pbInput), dwInputLength);
    }
}
KRNL_FUNC(XcSHAUpdate, &XcSHAUpdate);

// XcSHAFinal - slot 29, Ord 337, stdcall, 2 args (purge 8): (PUCHAR pbSHAContext, PUCHAR pbDigest[20]).
// Evidence: kernel_imports.tsv; FUN_000f6f88 (inner digest, then re-init of the same context),
// FUN_000f902f. The context stays usable only after a new XcSHAInit (as on the Xbox).
void __stdcall XcSHAFinal(unsigned char* pbSHAContext, unsigned char* pbDigest) noexcept {
    if (xc::ShaContext* ctx = GuestContext(pbSHAContext, "XcSHAFinal")) {
        if (pbDigest == nullptr) {
            FUT_LOGE(Krnl, "XcSHAFinal(%p, NULL): NULL digest", static_cast<void*>(pbSHAContext));
            return;
        }
        xc::ShaFinal(*ctx, pbDigest);
        FUT_LOGT(Krnl, "XcSHAFinal(%p) -> %02X%02X%02X%02X...", static_cast<void*>(pbSHAContext), pbDigest[0],
                 pbDigest[1], pbDigest[2], pbDigest[3]);
    }
}
KRNL_FUNC(XcSHAFinal, &XcSHAFinal);

// XcVerifyPKCS1Signature - slot 72, Ord 344, stdcall, 3 args (purge 12):
// BOOLEAN (PUCHAR pbSig, PUCHAR pbPubKey, PUCHAR pbDigest).
// Evidence: kernel_imports.tsv; only caller FUN_000f902f (DVD check from FUN_000f90e6, called with
// XePublicKeyData); XapiInitProcess ignores the result (0x0F931C/0x0F931E, kernel.md 7.5).
// Design 2.1: always TRUE.
BOOLEAN __stdcall XcVerifyPKCS1Signature(const unsigned char* pbSig, const unsigned char* pbPubKey,
                                         const unsigned char* pbDigest) noexcept {
    FUT_LOGD(Krnl, "XcVerifyPKCS1Signature(sig %p, key %p, digest %p) -> TRUE (design 2.1)",
             static_cast<const void*>(pbSig), static_cast<const void*>(pbPubKey),
             static_cast<const void*>(pbDigest));
    return TRUE;
}
KRNL_FUNC(XcVerifyPKCS1Signature, &XcVerifyPKCS1Signature);

}  // namespace krnl
