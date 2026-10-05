// HostMain stage "InitVfs" (design 1.10 step 3, 4.1): builds the device part of the object namespace.
// The symbolic links (\??\D:, \??\T:, \??\U:, \??\Z:) are created later by the guest itself
// (XapiInitProcess / XapiMapLetterToDirectory via IoCreateSymbolicLink, files.md 1.1) and by the
// XMountUtilityDrive hook (core).
#include "vfs/vfs_init.h"

#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

#include "core/log.h"
#include "core/trace.h"

#include <system_error>

namespace vfs {

bool StageInitVfs(const core::Config& config) {
    try {
        // --trace=krnl / --trace=vfs (design 5.4): kernel trace ring + crash reporter, vfs_trace.log.
        // A trace file that cannot be created is logged but does not stop the run.
        core::trace::Init(config);

        // \Device\CdRom0 (D:): the disc image, optionally an extracted disc directory, else no files.
        std::shared_ptr<Device> cdrom;
        const std::filesystem::path discDir = config.GetPath("paths", "disc_dir");
        if (!config.iso.empty()) {
            auto image = std::make_shared<XdvdfsImage>();
            std::string error;
            if (!image->Open(config.iso, error)) {
                FUT_LOGE(Vfs, "disc image %s: %s", core::WideToUtf8(config.iso.wstring()).c_str(),
                         error.c_str());
                return false;
            }
            FUT_LOGI(Vfs, "disc image %s: XDVDFS at offset 0x%llX, %u entries",
                     core::WideToUtf8(config.iso.wstring()).c_str(),
                     static_cast<unsigned long long>(image->PartitionOffset()), image->EntryCount());
            cdrom = CreateIsoDevice(std::move(image));
        } else if (!discDir.empty()) {
            // Design 4.1: optional DirDevice for an extracted disc (read-only).
            cdrom = CreateHostDirDevice(discDir, true);
            if (!cdrom) {
                FUT_LOGE(Vfs, "[paths] disc_dir %s is not usable",
                         core::WideToUtf8(discDir.wstring()).c_str());
                return false;
            }
        } else {
            FUT_LOGW(Vfs, "no disc image configured ([paths] iso): %s has no files", kCdRom0);
            cdrom = detail::CreateEmptyDevice("EmptyDevice");
        }
        if (!cdrom || !AddDevice(kCdRom0, cdrom)) {
            FUT_LOGE(Vfs, "cannot register %s", kCdRom0);
            return false;
        }

        // \Device\Harddisk0\partition1 (T:, U:) = <userdata>\hdd\partition1. Design 4.1: TDATA and
        // UDATA exist on a formatted Xbox HDD; XAPI only creates the title directories below them.
        const std::filesystem::path partition1 = config.Partition1Directory();
        for (const char* sub : {"TDATA", "UDATA"}) {
            std::string error;
            if (!CreateHostDirectories(partition1 / sub, error)) {  // "\\?\" form: no MAX_PATH limit
                FUT_LOGE(Vfs, "cannot create %s: %s", core::WideToUtf8((partition1 / sub).wstring()).c_str(),
                         error.c_str());
                return false;
            }
        }
        std::shared_ptr<Device> hdd = CreateHostDirDevice(partition1, false);
        if (!hdd || !AddDevice(kPartition1, hdd)) {
            FUT_LOGE(Vfs, "cannot register %s (%s)", kPartition1,
                     core::WideToUtf8(partition1.wstring()).c_str());
            return false;
        }
        FUT_LOGI(Vfs, "%s = %s", kPartition1, core::WideToUtf8(partition1.wstring()).c_str());
        return true;
    } catch (const std::exception& e) {
        FUT_LOGE(Vfs, "stage InitVfs: %s", e.what());
        return false;
    }
}

}  // namespace vfs
