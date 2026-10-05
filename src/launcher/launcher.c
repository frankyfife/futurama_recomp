/* futurama.exe - launcher fixed at 0x10000 (design 1.3, E1; launcher_probe.md).
 *
 * Linked WITHOUT CRT (/NODEFAULTLIB /ENTRY:LauncherEntry), no TLS, no __declspec(thread).
 * Link options are set on the linker command line by src/launcher/CMakeLists.txt (not via #pragma,
 * which ignores /FIXED etc. with LNK4229):
 *   /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO /INCREMENTAL:NO
 *   /NODEFAULTLIB /ENTRY:LauncherEntry /SUBSYSTEM:WINDOWS kernel32.lib user32.lib
 *
 * Sequence (design 1.3 item 3):
 *   1. TlsAlloc x5 (S_TLS, S_KTHREAD, S_PRCB, S_IRQL, S_DPC) before any LoadLibrary; each index < 64.
 *   2. Check the XBE placeholder: &xbe_space == 0x11000 and 0x11000 + size >= 0x31A2F8.
 *   3. LoadLibraryExW(<exe dir>\futurama_host.dll), GetProcAddress("HostMain").
 *   4. ExitProcess(HostMain(&info)).
 * Keep this file tiny: no large stack frames (no __chkstk without CRT), no struct initializers that
 * could turn into memset calls, no floating point.
 */
#include <windows.h>

#include "shared/launcher_info.h"

#if defined(__clang__)
/* clang-cl puts code into ".text" (cl.exe: ".text$mn"). When the linker merges the grouped sections,
 * ".text" sorts before ".text$a" and would push the placeholder off 0x11000 (seen with lld-link:
 * 0x11500). Keep the launcher code behind the placeholder explicitly (review finding 6). */
#pragma code_seg(".text$mn")
#endif

/* Defined in placeholder.asm (.text$a). */
extern unsigned char xbe_space[];
extern unsigned char xbe_space_end[];

static LauncherInfo g_info; /* zero-initialised (.bss) */
static wchar_t g_exePath[MAX_PATH * 4];
static wchar_t g_dllPath[MAX_PATH * 4 + 32];
static char g_msg[1024];

static void Fail(int exitCode, const char* msg) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    OutputDebugStringA(msg);
    if (err == NULL || err == INVALID_HANDLE_VALUE ||
        !WriteFile(err, msg, (DWORD)lstrlenA(msg), &written, NULL)) {
        /* No console/pipe (double-click start): show the error. */
        MessageBoxA(NULL, msg, "Futurama launcher", MB_OK | MB_ICONERROR);
    }
    ExitProcess((UINT)exitCode);
}

void __cdecl LauncherEntry(void) {
    static const char* const slotNames[FUT_TLS_SLOT_COUNT] = {"S_TLS", "S_KTHREAD", "S_PRCB", "S_IRQL",
                                                              "S_DPC"};
    HostMainFn hostMain;
    HMODULE host;
    DWORD i;
    DWORD len;
    DWORD space;
    DWORD spaceSize;
    int rc;

    /* No WER / critical-error dialogs: the host writes its own crash logs and minidumps. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    /* 1. TLS slots first, before any DLL can take low indices (design 1.3 item 3.1, cpu.md 2.4 risk a). */
    for (i = 0; i < FUT_TLS_SLOT_COUNT; ++i) {
        DWORD index = TlsAlloc();
        if (index == TLS_OUT_OF_INDEXES || index >= FUT_TEB_TLS_SLOTS_INLINE) {
            wsprintfA(g_msg, "launcher: TlsAlloc for %s returned %lu (must be < %u)\r\n", slotNames[i],
                      (unsigned long)index, FUT_TEB_TLS_SLOTS_INLINE);
            Fail(FUT_EXIT_LAUNCHER_TLS, g_msg);
        }
        TlsSetValue(index, NULL);
        g_info.tlsSlots[i] = index;
    }

    /* 2. Image base and placeholder (design 1.3 item 2). */
    if ((DWORD)(DWORD_PTR)GetModuleHandleW(NULL) != FUT_LAUNCHER_IMAGE_BASE) {
        wsprintfA(g_msg, "launcher: image base is %p, expected 0x%08lX (/BASE /FIXED missing?)\r\n",
                  (void*)GetModuleHandleW(NULL), (unsigned long)FUT_LAUNCHER_IMAGE_BASE);
        Fail(FUT_EXIT_LAUNCHER_BASE, g_msg);
    }
    space = (DWORD)(DWORD_PTR)xbe_space;
    spaceSize = (DWORD)((DWORD_PTR)xbe_space_end - (DWORD_PTR)xbe_space);
    if (space != FUT_XBE_SPACE_ADDRESS || space + spaceSize < FUT_XBE_SPACE_MIN_END) {
        wsprintfA(g_msg, "launcher: XBE placeholder at 0x%08lX size 0x%08lX, expected 0x%08lX..>=0x%08lX\r\n",
                  (unsigned long)space, (unsigned long)spaceSize, (unsigned long)FUT_XBE_SPACE_ADDRESS,
                  (unsigned long)FUT_XBE_SPACE_MIN_END);
        Fail(FUT_EXIT_LAUNCHER_PLACEHOLDER, g_msg);
    }

    /* 3. Host DLL from the directory of the exe (full path, its dependencies searched there too). */
    len = GetModuleFileNameW(NULL, g_exePath, (DWORD)(sizeof(g_exePath) / sizeof(g_exePath[0])));
    if (len == 0 || len >= (DWORD)(sizeof(g_exePath) / sizeof(g_exePath[0]))) {
        Fail(FUT_EXIT_LAUNCHER_LOAD_DLL, "launcher: GetModuleFileNameW failed\r\n");
    }
    lstrcpyW(g_dllPath, g_exePath);
    for (i = len; i > 0; --i) {
        if (g_dllPath[i - 1] == L'\\' || g_dllPath[i - 1] == L'/') {
            break;
        }
    }
    g_dllPath[i] = 0;
    lstrcatW(g_dllPath, FUT_HOST_DLL_NAME);

    host = LoadLibraryExW(g_dllPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (host == NULL) {
        wsprintfA(g_msg, "launcher: cannot load %ls (error %lu)\r\n", g_dllPath, GetLastError());
        Fail(FUT_EXIT_LAUNCHER_LOAD_DLL, g_msg);
    }
    hostMain = (HostMainFn)GetProcAddress(host, FUT_HOST_MAIN_EXPORT);
    if (hostMain == NULL) {
        Fail(FUT_EXIT_LAUNCHER_NO_HOSTMAIN, "launcher: futurama_host.dll does not export HostMain\r\n");
    }

    /* 4. Hand over (design 1.3 item 3.4); HostMain returns only at process end. */
    g_info.size = sizeof(g_info);
    g_info.version = FUT_LAUNCHER_INFO_VERSION;
    g_info.xbeSpace = space;
    g_info.xbeSpaceSize = spaceSize;
    g_info.imageBase = FUT_LAUNCHER_IMAGE_BASE;
    g_info.cmdline = GetCommandLineW();
    g_info.exePath = g_exePath;
    g_info.launcherModule = (void*)GetModuleHandleW(NULL);
    rc = hostMain(&g_info);
    ExitProcess((UINT)rc);
}
