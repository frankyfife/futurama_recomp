// Calling guest (XBE) code from host C++ (design 3.3). Owned by the core agent.
//
// Two ways:
//  1. Typed function pointers (fine for ordinary stdcall/cdecl/fastcall/thiscall guest functions):
//       guest::Func<BOOL(__stdcall*)(const void*, UINT_PTR)>(guest::kXapiIsBadReadPtr)(p, n);
//  2. The MASM helpers below (src/host/asm/guest_call.asm) when exact register/stack control matters
//     (the XBE entry point, SystemRoutine of PsCreateSystemThreadEx, calls whose purge is unknown):
//     they preserve ebx/esi/edi/ebp, restore esp afterwards (callee- and caller-cleanup both work)
//     and return edx:eax.
// The calling thread must have been adopted (core::AdoptCurrentThread) before any guest code runs.
#pragma once

#include "core/guest.h"

#include <cstdint>
#include <initializer_list>

extern "C" {
std::uint32_t __cdecl fut_guest_call0(std::uint32_t fn);
std::uint64_t __cdecl fut_guest_call(std::uint32_t fn, const std::uint32_t* args, std::uint32_t count);
std::uint64_t __cdecl fut_guest_call_reg(std::uint32_t fn, std::uint32_t ecxValue, std::uint32_t edxValue,
                                         const std::uint32_t* args, std::uint32_t count);
}

namespace core {

// fn(args...) with all DWORD arguments on the stack (stdcall or cdecl). Returns eax.
inline std::uint32_t CallGuest(guest::GuestAddr fn, std::initializer_list<std::uint32_t> args = {}) noexcept {
    return static_cast<std::uint32_t>(
        fut_guest_call(fn, args.size() ? args.begin() : nullptr, static_cast<std::uint32_t>(args.size())));
}
// Same, returning edx:eax (64-bit results).
inline std::uint64_t CallGuest64(guest::GuestAddr fn,
                                 std::initializer_list<std::uint32_t> args = {}) noexcept {
    return fut_guest_call(fn, args.size() ? args.begin() : nullptr, static_cast<std::uint32_t>(args.size()));
}
// thiscall: ecx = self, other DWORD arguments on the stack.
inline std::uint32_t CallGuestThiscall(guest::GuestAddr fn, std::uint32_t self,
                                       std::initializer_list<std::uint32_t> args = {}) noexcept {
    return static_cast<std::uint32_t>(fut_guest_call_reg(fn, self, 0, args.size() ? args.begin() : nullptr,
                                                         static_cast<std::uint32_t>(args.size())));
}
// fastcall: ecx, edx, remaining DWORD arguments on the stack.
inline std::uint32_t CallGuestFastcall(guest::GuestAddr fn, std::uint32_t ecxValue, std::uint32_t edxValue,
                                       std::initializer_list<std::uint32_t> stackArgs = {}) noexcept {
    return static_cast<std::uint32_t>(fut_guest_call_reg(fn, ecxValue, edxValue,
                                                         stackArgs.size() ? stackArgs.begin() : nullptr,
                                                         static_cast<std::uint32_t>(stackArgs.size())));
}

}  // namespace core
