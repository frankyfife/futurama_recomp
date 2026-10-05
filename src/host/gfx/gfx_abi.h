// Compile-time stack purge of host replacement functions (design 3.3: "Purge per static_assert
// prüfen"). Used by the D3D shim (stdcall) and the NiPCRenderer slot handlers (thiscall guest calls
// implemented as `__fastcall(Self*, void* edx, stack args...)`: ecx = this, edx ignored, the callee
// pops the stack arguments exactly like the original `ret N`).
#pragma once

#include <cstdint>
#include <type_traits>

namespace gfx::abi {

template <class T>
inline constexpr std::uint32_t kStackBytes = (static_cast<std::uint32_t>(sizeof(T)) + 3u) & ~3u;

template <class T>
inline constexpr bool kRegisterEligible =
    (std::is_integral_v<T> || std::is_pointer_v<T> || std::is_enum_v<T>) && sizeof(T) <= 4;

template <class F>
struct Purge {
    static constexpr bool stdcall = false;
    static constexpr bool thiscallLike = false;
    static constexpr std::uint32_t bytes = 0;
};
// __stdcall: every argument on the stack.
template <class R, class... A>
struct Purge<R(__stdcall*)(A...) noexcept> {
    static constexpr bool stdcall = true;
    static constexpr bool thiscallLike = false;
    static constexpr std::uint32_t bytes = (0u + ... + kStackBytes<A>);
};
// __fastcall(this, edx, args...): the two register arguments must be DWORD-sized integers/pointers,
// everything after them is on the stack (MSVC/clang-cl x86 __fastcall rules).
template <class R, class T, class E, class... A>
struct Purge<R(__fastcall*)(T, E, A...) noexcept> {
    static constexpr bool stdcall = false;
    static constexpr bool thiscallLike = kRegisterEligible<T> && kRegisterEligible<E>;
    static constexpr std::uint32_t bytes = (0u + ... + kStackBytes<A>);
};

// Stack purge in bytes of a stdcall function pointer type (static_assert helper).
template <class F>
constexpr std::uint32_t StdcallPurge() {
    static_assert(Purge<F>::stdcall, "expected a __stdcall noexcept function");
    return Purge<F>::bytes;
}
// Number of stack DWORDs of a thiscall-style __fastcall(Self*, void*, ...) noexcept handler.
template <class F>
constexpr std::uint32_t ThiscallStackArgs() {
    static_assert(Purge<F>::thiscallLike, "expected __fastcall(Self*, void* edx, stack args...) noexcept");
    return Purge<F>::bytes / 4;
}

}  // namespace gfx::abi
