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
};

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
    {kPartition1, "\\UDATA\\46490002\\*",
     "save game not created yet (fresh userdata): XCreateSaveGame/XFindFirstSaveGame/the save layer look "
     "up U:\\<12-hex-hash>\\... before the first save (files.md 4.1/4.2)"},
};

}  // namespace

const char* DocumentedMissReason(std::string_view devicePath, std::string_view relativePath) noexcept {
    for (const KnownMiss& k : kKnownMisses) {
        if (IEquals(devicePath, k.device) && MatchFileMask(relativePath, k.pattern)) {
            return k.reason;
        }
    }
    return nullptr;
}

}  // namespace vfs::detail
