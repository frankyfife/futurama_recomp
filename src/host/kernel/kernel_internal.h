// Internal interfaces between the kernel HLE translation units (not for other modules).
// Owned by the kernel agent.
#pragma once

#include "kernel/kernel_exports.h"
#include "kernel/kernel_settings.h"
#include "shared/xbox_types.h"

#include <cstdint>

namespace krnl {

// rtl_status.cpp: the RtlNtStatusToDosError table (self-test / unit tests).
std::uint32_t StatusTableSize() noexcept;
void StatusTableEntry(std::uint32_t index, std::uint32_t& status, std::uint32_t& error) noexcept;

// data_exports.cpp: fills the DATA exports from the settings (keys, AV pack). Called by
// StageInitKernel (and by tests). Returns false if a guest-visible object lies above 0x80000000.
bool InitDataExports(const KernelSettings& settings) noexcept;
// Logs the data/ADDR export values (debug).
void LogDataExports() noexcept;
// Current key material (16 bytes each; alternate: 16 x 16 bytes).
const std::uint8_t* DataXboxHDKey() noexcept;
const std::uint8_t* DataXboxSignatureKey() noexcept;
const std::uint8_t* DataXboxAlternateSignatureKeys() noexcept;

// ke_thread.cpp: creates the thread-exit event. Called by StageInitKernel (idempotent).
bool InitThreading() noexcept;

// ke_irql.cpp: dispatcher lock used by the IRQL emulation (design 1.7).
void InitIrql() noexcept;

}  // namespace krnl
