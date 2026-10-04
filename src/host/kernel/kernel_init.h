// Kernel HLE initialization. Owned by the kernel agent.
#pragma once

#include "core/config.h"

namespace krnl {

// HostMain stage "InitKernel" (runs before the thunk table is resolved): prepare data exports from
// the configuration (XboxSignatureKey, language for ExQueryNonVolatileSetting, ...), allocate global
// HLE state. Must not run guest code.
bool StageInitKernel(const core::Config& config);

}  // namespace krnl
