// Documented misses for the VFS trace (--trace=vfs, M2 exit criterion 7: "jede Oeffnung ist ein Treffer
// oder ein dokumentiertes 'fehlt'"). Owned by the io agent.
//
// Every entry is a name the game looks up although it does not exist on the retail EU disc or on a
// fresh partition1, with the analysis that explains it. Patterns are device-relative, matched
// case-insensitively with '*' / '?' (vfs::MatchFileMask; '*' also spans '\'). A miss that matches no
// entry is reported as "missing UNDOCUMENTED" in vfs_trace.log and must be analysed (and added here).
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

namespace vfs::detail {
namespace {

struct KnownMiss {
    const char* device;  // kCdRom0 / kPartition1
    const char* pattern;
    const char* reason;
    bool saveDirectory = false;  // the 12 '?' after \UDATA\46490002\ must be hex digits (a save hash)
};

constexpr char kSaveReason[] =
    "save game not created yet (fresh userdata): XCreateSaveGame/the save layer look up "
    "U:\\<12-hex-hash>\\... before the first save (files.md 4.1/4.2)";

constexpr KnownMiss kKnownMisses[] = {
    // --- disc (\Device\CdRom0 = D:) ---
    {kCdRom0, "\\FutPAL05.img",
     "end of the FutPAL%02d.img mount loop: FUN_000D26A0 mounts 00..99 until the first miss, the disc "
     "has 00..04 (files.md 3.2, 5.5 item 3)"},
    {kCdRom0, "\\sysNTSC.img", "NTSC fallback, only tried without sysPAL.img (files.md 3.2)"},
    {kCdRom0, "\\FutNTSC??.img", "NTSC fallback, only tried without FutPAL00.img (files.md 3.2)"},
    {kCdRom0, "\\Video\\*.txt",
     "optional start-marker table Video/%s.txt (FUN_000ED8B0 via FUN_0013FB10), exists nowhere "
     "(files.md 1.2)"},
    {kCdRom0, "\\fonts\\kanjii.fnt",
     "font referenced by FUN_000985A0 (0x988DC) but in no archive and not on the disc (files.md 1.2)"},
    {kCdRom0, "\\fonts\\kanjii.nif",
     "font referenced by code but in no archive and not on the disc (files.md 1.2)"},
    {kCdRom0, "\\fonts\\arial2t20px32x32.fnt",
     "font referenced by code but in no archive and not on the disc (files.md 1.2)"},
    {kCdRom0, "\\fonts\\arial2t20px32x32.nif",
     "font referenced by code but in no archive and not on the disc (files.md 1.2)"},
    {kCdRom0, "\\sounds\\menu\\MusicClip.wav",
     "asset referenced by code but missing from the archives (files.md 1.2)"},
    {kCdRom0, "\\Sounds\\Mine_Activate.wav",
     "asset referenced by code but missing from the archives (files.md 1.2)"},
    {kCdRom0, "\\Export\\Chars\\LipsyncMeshes\\*",
     "development path, not on the disc (FUN_000D3F40, files.md 1.2)"},
    {kCdRom0, "\\NiTCache.dat", "inactive NiXBox texture cache (DAT_0030EDD4 is never set, files.md 1.2)"},
    // --- partition1 (T:, U:) ---
    // Only the save directories (12 hex digits = 48-bit hash of the save name, FUN_000F6544) and the five
    // files a save contains (files.md 4.1/4.2). Anything else under U: (e.g. TitleMeta.xbx) must exist
    // (M2 review finding 8: the former pattern \UDATA\46490002\* documented every miss under U:).
    {kPartition1, "\\UDATA\\46490002\\????????????", kSaveReason, true},
    {kPartition1, "\\UDATA\\46490002\\????????????\\SaveMeta.xbx", kSaveReason, true},
    {kPartition1, "\\UDATA\\46490002\\????????????\\Futurama.dat", kSaveReason, true},
    {kPartition1, "\\UDATA\\46490002\\????????????\\Options.dat", kSaveReason, true},
    {kPartition1, "\\UDATA\\46490002\\????????????\\Unlock.dat", kSaveReason, true},
    {kPartition1, "\\UDATA\\46490002\\????????????\\saveimage.xbx", kSaveReason, true},
};

constexpr std::string_view kSaveRoot = "\\UDATA\\46490002\\";
constexpr std::size_t kSaveHashChars = 12;

// The 12 characters after \UDATA\46490002\ are hex digits (the pattern already fixed the length).
bool IsSaveDirectoryPath(std::string_view relativePath) noexcept {
    if (relativePath.size() < kSaveRoot.size() + kSaveHashChars) {
        return false;
    }
    for (std::size_t i = kSaveRoot.size(); i < kSaveRoot.size() + kSaveHashChars; ++i) {
        const char c = relativePath[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* DocumentedMissReason(std::string_view devicePath, std::string_view relativePath) noexcept {
    for (const KnownMiss& k : kKnownMisses) {
        if (IEquals(devicePath, k.device) && MatchFileMask(relativePath, k.pattern) &&
            (!k.saveDirectory || IsSaveDirectoryPath(relativePath))) {
            return k.reason;
        }
    }
    return nullptr;
}

}  // namespace vfs::detail
