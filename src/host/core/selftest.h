// Registry of named self-tests (design 1.4 step 8; enabled with --selftest / [debug] selftest=1).
//
//   FUT_SELFTEST(TlsSlotsReadBack, Host) {
//       // return true on success; put a short explanation into `detail` (logged on failure and success)
//       detail = "5 slots ok";
//       return true;
//   }
//
// Phases (who runs them is decided by core/host_main.cpp + core/boot.cpp):
//   Host            needs no mapped XBE (TLS slot read-back via __readfsdword, CreateThread with 0 stack)
//   Mapped          after mapping, polyglot, thunks, fs patches and hooks (mapping vs. file, BSS = 0,
//                   RtlImageNtHeader after the polyglot)
//   AdoptedThread   on the adopted boot thread right before entry (guest IsBadReadPtr(0,4) == TRUE)
// StageRunSelfTests runs Host + Mapped; core/boot.cpp runs AdoptedThread.
#pragma once

#include "core/config.h"

#include <cstdint>
#include <string>

namespace core {

enum class SelfTestPhase : std::uint8_t { Host, Mapped, AdoptedThread };

using SelfTestFn = bool (*)(std::string& detail);

struct SelfTestRegistrar {
    SelfTestRegistrar(const char* name, SelfTestPhase phase, SelfTestFn fn, const char* file) noexcept;
};

// Runs every test of `phase` (catching exceptions), logs "SELFTEST PASS|FAIL <name>: <detail>" and
// returns the number of failures. Does nothing (returns 0) unless enabled by the configuration.
int RunSelfTests(SelfTestPhase phase) noexcept;
// Number of registered tests in a phase.
int SelfTestCount(SelfTestPhase phase) noexcept;
// (M2 review) True while RunSelfTests executes a test on the calling thread. Lets statistics and traces
// keep the self-tests' own activity apart from the game's (fiber switches, VFS trace lines).
bool InSelfTest() noexcept;

// HostMain stage (after hooks): runs the Host and Mapped tests; false if any failed.
bool StageRunSelfTests(const Config& config);

}  // namespace core

#define FUT_SELFTEST(id, phase)                                                                              \
    static bool futSelfTest_##id(std::string& detail);                                                       \
    namespace {                                                                                              \
    const ::core::SelfTestRegistrar futSelfTestRegistrar_##id(#id, ::core::SelfTestPhase::phase,             \
                                                              &futSelfTest_##id, __FILE__);                  \
    }                                                                                                        \
    static bool futSelfTest_##id([[maybe_unused]] std::string& detail)
