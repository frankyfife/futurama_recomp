// Kernel HLE configuration parsing. See kernel_settings.h. Owned by the kernel agent.
#include "kernel/kernel_settings.h"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace krnl {
namespace {

KernelSettings g_settings{};  // written once by StageInitKernel before guest code runs

struct LanguageEntry {
    const char* name;
    std::uint32_t value;
};
// XC_LANGUAGE_* values (ASSUMPTION: XDK numbering; the guest only distinguishes 3/4/5/6 and
// "everything else = english", files.md 4.5 / FUN_000d9990).
constexpr LanguageEntry kLanguages[] = {
    {"english", 1}, {"japanese", 2}, {"german", 3},   {"french", 4},     {"spanish", 5},
    {"italian", 6}, {"korean", 7},   {"tchinese", 8}, {"portuguese", 9},
};

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

bool ParseU32(std::string_view text, std::uint32_t& out) {
    std::string s(text);
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

bool ParseKey16(std::string_view text, std::array<std::uint8_t, 16>& out) {
    std::string hex;
    for (char c : text) {
        if (c == ' ' || c == '-' || c == ':') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        hex.push_back(c);
    }
    if (hex.size() != 32) {
        return false;
    }
    for (std::size_t i = 0; i < 16; ++i) {
        const std::string byte = hex.substr(2 * i, 2);
        out[i] = static_cast<std::uint8_t>(std::strtoul(byte.c_str(), nullptr, 16));
    }
    return true;
}

bool GetU32Value(const core::Config& config, std::string_view section, std::string_view key,
                 std::uint32_t& value, std::string& error) {
    if (!config.Has(section, key)) {
        return true;
    }
    const std::string text = config.GetString(section, key);
    if (!ParseU32(text, value)) {
        error = "[" + std::string(section) + "] " + std::string(key) + " = '" + text + "' is not a number";
        return false;
    }
    return true;
}

}  // namespace

const char* LanguageName(std::uint32_t language) noexcept {
    for (const auto& l : kLanguages) {
        if (l.value == language) {
            return l.name;
        }
    }
    return nullptr;
}

bool LoadKernelSettings(const core::Config& config, KernelSettings& out, std::string& error) {
    KernelSettings s{};

    if (config.Has("settings", "language")) {
        const std::string text = Lower(config.GetString("settings", "language"));
        bool found = false;
        for (const auto& l : kLanguages) {
            if (text == l.name) {
                s.language = l.value;
                found = true;
            }
        }
        if (!found) {
            std::uint32_t v = 0;
            if (!ParseU32(text, v) || v < 1 || v > 9) {
                error = "[settings] language = '" + text +
                        "' (expected english|german|french|spanish|italian|... or 1..9)";
                return false;
            }
            s.language = v;
        }
    }
    if (!GetU32Value(config, "settings", "game_region", s.gameRegion, error) ||
        !GetU32Value(config, "settings", "av_region", s.avRegion, error) ||
        !GetU32Value(config, "settings", "video_flags", s.videoFlags, error) ||
        !GetU32Value(config, "settings", "audio_flags", s.audioFlags, error) ||
        !GetU32Value(config, "settings", "av_pack", s.avPack, error) ||
        !GetU32Value(config, "kernel", "min_guest_stack", s.minGuestStack, error)) {
        return false;
    }
    if (s.minGuestStack < 0x10000 || s.minGuestStack > 0x4000000) {
        error = "[kernel] min_guest_stack must be between 0x10000 and 0x4000000";
        return false;
    }

    const std::string mode = Lower(config.GetString("saves", "key_mode", "fixed"));
    if (mode == "fixed") {
        s.keyMode = SaveKeyMode::Fixed;
    } else if (mode == "xbox") {
        s.keyMode = SaveKeyMode::Xbox;
    } else {
        error = "[saves] key_mode = '" + mode + "' (expected fixed or xbox)";
        return false;
    }
    if (config.Has("saves", "signature_key")) {
        if (!ParseKey16(config.GetString("saves", "signature_key"), s.signatureKeyOverride)) {
            error = "[saves] signature_key must be 32 hex digits";
            return false;
        }
        s.hasSignatureKeyOverride = true;
    }
    if (config.Has("saves", "xbox_cert_key")) {
        if (!ParseKey16(config.GetString("saves", "xbox_cert_key"), s.xboxCertKey)) {
            error = "[saves] xbox_cert_key must be 32 hex digits";
            return false;
        }
        s.hasXboxCertKey = true;
    }
    if (s.keyMode == SaveKeyMode::Xbox && !s.hasXboxCertKey) {
        error = "[saves] key_mode = xbox needs [saves] xbox_cert_key (design 4.5)";
        return false;
    }
    out = s;
    return true;
}

const KernelSettings& Settings() noexcept {
    return g_settings;
}

void SetSettings(const KernelSettings& settings) noexcept {
    g_settings = settings;
}

}  // namespace krnl
