#include <windows.h>
#include <stdio.h>
int main() {
    // Can a 32-bit process reserve the XBE range at 0x10000 (image ~0x31A2F8 bytes)?
    void* p = VirtualAlloc((void*)0x10000, 0x31B000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    printf("reserve @0x10000: %p (err %lu)\n", p, p ? 0 : GetLastError());
    // Can we get memory above 2 GB (Xbox contiguous memory is addressed at 0x80000000+)?
    void* q = VirtualAlloc((void*)0x80000000, 0x4000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    printf("reserve @0x80000000: %p (err %lu)\n", q, q ? 0 : GetLastError());
    printf("image base: %p\n", GetModuleHandleA(NULL));
    return 0;
}
