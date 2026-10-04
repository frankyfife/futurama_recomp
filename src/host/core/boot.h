// Boot thread adoption and the call of the XBE entry point (design 1.10 step 5). Owned by the core agent.
#pragma once

#include "core/config.h"

namespace core {

// Stage "AdoptBootThreadAndRunEntry": core::AdoptCurrentThread(0, "boot") (TlsData = 0, dummy TLS
// anchor), run the AdoptedThread self-tests, install the --stop-at hook, call entry 0xF84DD (via a
// MASM thunk in src/host/asm). entry creates the guest main thread through PsCreateSystemThreadEx
// and returns. Returns false if entry could not be run.
bool StageAdoptBootThreadAndRunEntry(const Config& config);

}  // namespace core
