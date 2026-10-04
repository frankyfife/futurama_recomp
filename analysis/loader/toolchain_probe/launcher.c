// Probe: can a launcher linked at 0x10000 own the whole XBE address range?
// The XBE image (retail EU) spans 0x10000..0x31A2F8; its header must live at 0x10000.
#include <windows.h>

#pragma comment(linker, "/ENTRY:start /SUBSYSTEM:CONSOLE")

// Placeholder for XBE sections 0x11000.. (uninitialised, so it costs no file space).
extern unsigned char xbe_space[0x30A000];  // defined in placeholder.asm

static void print(const char* s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, lstrlenA(s), &n, NULL);
}

void start(void) {
    char buf[256];
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old;
    VirtualQuery((void*)0x10000, &mbi, sizeof(mbi));
    wsprintfA(buf, "image base %p, xbe_space %p..%p\n", GetModuleHandleA(NULL), xbe_space,
              xbe_space + sizeof(xbe_space));
    print(buf);
    // The XBE header must be written over our PE header page; check we may make it writable.
    BOOL ok = VirtualProtect((void*)0x10000, 0x1000, PAGE_EXECUTE_READWRITE, &old);
    wsprintfA(buf, "VirtualProtect header page: %s\n", ok ? "ok" : "failed");
    print(buf);
    ok = VirtualProtect(xbe_space, sizeof(xbe_space), PAGE_EXECUTE_READWRITE, &old);
    wsprintfA(buf, "VirtualProtect xbe_space RWX: %s\n", ok ? "ok" : "failed");
    print(buf);
    // Executing code in the placeholder (what the XBE .text will need).
    xbe_space[0] = 0xB8; *(DWORD*)&xbe_space[1] = 0x1234; xbe_space[5] = 0xC3;  // mov eax,0x1234; ret
    DWORD r = ((DWORD(*)(void))xbe_space)();
    wsprintfA(buf, "executed code in xbe_space: %s\n", r == 0x1234 ? "ok" : "failed");
    print(buf);
    ExitProcess(0);
}

