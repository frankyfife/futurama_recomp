// NTSTATUS helpers shared by all host modules. The status codes themselves come from the Windows
// SDK <ntstatus.h> (included by shared/win32.h); Xbox and Windows NT use the same values.
#pragma once

#include "shared/win32.h"

namespace xbox {

// Symbolic name for logging (design 5.4: kernel trace decodes NTSTATUS names). Unknown codes -> nullptr.
constexpr const char* NtStatusName(NTSTATUS s) noexcept {
    switch (static_cast<unsigned long>(s)) {
    case 0x00000000: return "STATUS_SUCCESS";
    case 0x00000001: return "STATUS_WAIT_1";
    case 0x000000C0: return "STATUS_USER_APC";
    case 0x00000101: return "STATUS_ALERTED";
    case 0x00000102: return "STATUS_TIMEOUT";
    case 0x00000103: return "STATUS_PENDING";
    case 0x80000005: return "STATUS_BUFFER_OVERFLOW";
    case 0x80000006: return "STATUS_NO_MORE_FILES";
    case 0xC0000001: return "STATUS_UNSUCCESSFUL";
    case 0xC0000002: return "STATUS_NOT_IMPLEMENTED";
    case 0xC0000003: return "STATUS_INVALID_INFO_CLASS";
    case 0xC0000004: return "STATUS_INFO_LENGTH_MISMATCH";
    case 0xC0000005: return "STATUS_ACCESS_VIOLATION";
    case 0xC0000008: return "STATUS_INVALID_HANDLE";
    case 0xC000000D: return "STATUS_INVALID_PARAMETER";
    case 0xC000000F: return "STATUS_NO_SUCH_FILE";
    case 0xC0000010: return "STATUS_INVALID_DEVICE_REQUEST";
    case 0xC0000011: return "STATUS_END_OF_FILE";
    case 0xC0000017: return "STATUS_NO_MEMORY";
    case 0xC0000022: return "STATUS_ACCESS_DENIED";
    case 0xC0000023: return "STATUS_BUFFER_TOO_SMALL";
    case 0xC0000024: return "STATUS_OBJECT_TYPE_MISMATCH";
    case 0xC0000033: return "STATUS_OBJECT_NAME_INVALID";
    case 0xC0000034: return "STATUS_OBJECT_NAME_NOT_FOUND";
    case 0xC0000035: return "STATUS_OBJECT_NAME_COLLISION";
    case 0xC000003A: return "STATUS_OBJECT_PATH_NOT_FOUND";
    case 0xC0000043: return "STATUS_SHARING_VIOLATION";
    case 0xC000007F: return "STATUS_DISK_FULL";
    case 0xC000009A: return "STATUS_INSUFFICIENT_RESOURCES";
    case 0xC00000A2: return "STATUS_MEDIA_WRITE_PROTECTED";
    case 0xC00000BA: return "STATUS_FILE_IS_A_DIRECTORY";
    case 0xC00000BB: return "STATUS_NOT_SUPPORTED";
    case 0xC0000101: return "STATUS_DIRECTORY_NOT_EMPTY";
    case 0xC0000103: return "STATUS_NOT_A_DIRECTORY";
    case 0xC0000121: return "STATUS_CANNOT_DELETE";
    case 0xC0000144: return "STATUS_UNHANDLED_EXCEPTION";
    default: return nullptr;
    }
}

}  // namespace xbox
