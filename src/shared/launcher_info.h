/* Interface between the launcher (futurama.exe, C, no CRT) and the host DLL (futurama_host.dll).
 * Design: docs/loader_design.md 1.3 (LauncherEntry) and 1.6 (TLS slots for the fs: retargeting).
 *
 * The launcher allocates the five TLS slots BEFORE loading any DLL so that their indices are small
 * (< 64 => inline in the TEB at fs:[0xE10 + 4*index]), checks the XBE placeholder, loads
 * futurama_host.dll and calls HostMain(&info). HostMain returns the process exit code.
 */
#ifndef FUT_SHARED_LAUNCHER_INFO_H
#define FUT_SHARED_LAUNCHER_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Index into LauncherInfo.tlsSlots. Meaning per guest thread (design 1.6 table):
 *   S_TLS     : TlsData + TlsDataSize (TLS anchor; guest reads [anchor + (-9)*4])   <- fs:[0x04]
 *   S_KTHREAD : fake KTHREAD (xbox::FakeKThread)                                    <- fs:[0x28]
 *   S_PRCB    : fake KPRCB (xbox::FakeKPrcb, +0x250 must be 0)                       <- fs:[0x20]
 *   S_IRQL    : current IRQL (DWORD, normally 0; byte access reads the low byte)      <- fs:[0x24]
 *   S_DPC     : 0 (only DSOUND reads it)                                             <- fs:[0x58]
 */
enum FutTlsSlot {
    FUT_TLS_S_TLS = 0,
    FUT_TLS_S_KTHREAD = 1,
    FUT_TLS_S_PRCB = 2,
    FUT_TLS_S_IRQL = 3,
    FUT_TLS_S_DPC = 4,
    FUT_TLS_SLOT_COUNT = 5
};

/* TEB.TlsSlots[64] lives at fs:[0xE10]; only indices < 64 are addressable with a disp32. */
#define FUT_TEB_TLS_SLOTS_OFFSET 0xE10u
#define FUT_TEB_TLS_SLOTS_INLINE 64u

#define FUT_LAUNCHER_INFO_VERSION 1u

/* Placeholder for the XBE sections (placeholder.asm, section .text$a). */
#define FUT_XBE_SPACE_ADDRESS 0x00011000u
#define FUT_XBE_SPACE_SIZE 0x0030A000u
#define FUT_XBE_SPACE_MIN_END 0x0031A2F8u /* end of the last XBE section (.XTLID) */
#define FUT_LAUNCHER_IMAGE_BASE 0x00010000u

typedef struct LauncherInfo {
    unsigned long size;    /* sizeof(LauncherInfo) */
    unsigned long version; /* FUT_LAUNCHER_INFO_VERSION */
    unsigned long tlsSlots[FUT_TLS_SLOT_COUNT]; /* TlsAlloc indices, each < 64 */
    unsigned long xbeSpace;     /* address of the placeholder, == FUT_XBE_SPACE_ADDRESS */
    unsigned long xbeSpaceSize; /* size of the placeholder (from its end label) */
    unsigned long imageBase;    /* launcher image base, == FUT_LAUNCHER_IMAGE_BASE */
    const wchar_t* cmdline;     /* GetCommandLineW() of the process */
    const wchar_t* exePath;     /* full path of futurama.exe */
    void* launcherModule;       /* HMODULE of futurama.exe */
} LauncherInfo;

/* Exported by futurama_host.dll (undecorated name "HostMain"). Returns the process exit code. */
typedef int(__cdecl* HostMainFn)(const LauncherInfo* info);
#define FUT_HOST_DLL_NAME L"futurama_host.dll"
#define FUT_HOST_MAIN_EXPORT "HostMain"

/* Launcher exit codes (HostMain exit codes are listed in src/INTERFACES.md). */
#define FUT_EXIT_LAUNCHER_TLS 10         /* TlsAlloc failed or index >= 64 */
#define FUT_EXIT_LAUNCHER_PLACEHOLDER 11 /* placeholder not at 0x11000 or too small */
#define FUT_EXIT_LAUNCHER_LOAD_DLL 12    /* futurama_host.dll not loadable */
#define FUT_EXIT_LAUNCHER_NO_HOSTMAIN 13 /* export HostMain missing */
#define FUT_EXIT_LAUNCHER_BASE 14        /* launcher not loaded at 0x10000 */

#ifdef __cplusplus
}
#endif

#endif /* FUT_SHARED_LAUNCHER_INFO_H */
