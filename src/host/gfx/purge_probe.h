// Runtime measurement of the stack purge (`ret N`) of a callee-cleanup entry point, used by the gfx
// self-tests and unit tests to prove that every NiPCRenderer vtable thunk, D3D shim and D3D trap pops
// exactly the bytes the original guest function pops (design 3.2/3.3: a wrong purge destroys the
// guest stack).
//
// Method (no assembly needed): a host thread is created suspended; its context is set to
//   eip = entry, ecx/edx = given values, esp -> [return address = PurgeProbeLanding][N zero DWORDs]
// and resumed. The entry runs with InPurgeProbe() == true (every gfx entry point then returns at once
// without side effects) and executes its own `ret N`, which "returns" into PurgeProbeLanding. That
// function reads its entry esp via _AddressOfReturnAddress() and ends the thread with
// exit code = entry esp - (initial esp + 4) = the number of bytes the callee popped.
#pragma once

#include <cstdint>
#include <string>

namespace gfx {

// True on the probe thread of MeasureCalleePurge only (cheap: compares the current thread id).
bool InPurgeProbe() noexcept;

struct PurgeMeasurement {
    bool ok = false;
    std::uint32_t purgeBytes = 0;
    std::string error;  // why ok == false (thread creation, timeout, ...)
};

// Calls `entry` once on a probe thread as described above, with `stackDwords` zero arguments on the
// stack (plus 16 spare zero DWORDs, so a callee that pops too much still lands correctly). Serialized
// process-wide; waits at most 5 s. Never call it while the entry may run concurrently on a guest
// thread with real arguments (the probe mode is per thread, so that is harmless, but the result of a
// crashing entry is a process crash).
PurgeMeasurement MeasureCalleePurge(const void* entry, std::uint32_t ecx, std::uint32_t edx,
                                    std::uint32_t stackDwords) noexcept;

}  // namespace gfx
