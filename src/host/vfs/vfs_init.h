// VFS setup stage. Owned by the io agent.
#pragma once

#include "core/config.h"

namespace vfs {

// HostMain stage "InitVfs" (design 1.10 step 3): \Device\CdRom0 (IsoDevice from [paths] iso),
// \Device\Harddisk0\partition1 (HostDirDevice <userdata>\hdd\partition1, created with TDATA/UDATA),
// empty symbolic-link table.
bool StageInitVfs(const core::Config& config);

}  // namespace vfs
