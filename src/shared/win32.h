// Windows include policy for the host DLL (force-included into every host C++ translation unit by
// CMake, see src/host/CMakeLists.txt). Include this instead of <windows.h>.
//
// - WIN32_NO_STATUS + <ntstatus.h>: all NT status codes (STATUS_*) are available as NTSTATUS
//   constants, without the macro clashes winnt.h would otherwise produce.
// - NTSTATUS is a plain LONG, as on the Xbox and in the Windows DDK.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

typedef LONG NTSTATUS;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

static_assert(sizeof(void*) == 4, "the host must be built as 32-bit x86 code (guest pointers are host pointers)");
