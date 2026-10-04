; Host -> guest calls with exact register control (design 3.3: entry, SystemRoutine, self-tests).
; Owned by the core agent. C declarations: src/host/core/guest_call.h.
;
; All helpers
;  - preserve ebx/esi/edi/ebp for the C++ caller (MSVC ABI),
;  - push the arguments right to left and call the guest address,
;  - restore esp from ebp afterwards, so they work for callee-cleanup (stdcall/fastcall/thiscall) and
;    caller-cleanup (cdecl) guest functions alike and survive a wrong purge count,
;  - return eax in eax and edx in edx (a 64-bit C return value = edx:eax).
; They install no SEH frame: guest frames link into the Windows chain at fs:[0] as usual (design 1.11).
.686
.model flat, C
.code

; uint32_t __cdecl fut_guest_call0(uint32_t fn)
;   fn() - no arguments (XBE entry point 0xF84DD).
fut_guest_call0 PROC
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]
    call    eax
    lea     esp, [ebp-12]
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret
fut_guest_call0 ENDP

; uint64_t __cdecl fut_guest_call(uint32_t fn, const uint32_t* args, uint32_t count)
;   fn(args[0], ..., args[count-1]) with all arguments on the stack (stdcall or cdecl).
fut_guest_call PROC
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     ecx, [ebp+16]           ; count
    mov     esi, [ebp+12]           ; args
    test    ecx, ecx
    jz      call_now
push_next:
    push    dword ptr [esi+ecx*4-4] ; last argument first
    dec     ecx
    jnz     push_next
call_now:
    mov     eax, [ebp+8]
    call    eax
    lea     esp, [ebp-12]
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret
fut_guest_call ENDP

; uint64_t __cdecl fut_guest_call_reg(uint32_t fn, uint32_t ecxValue, uint32_t edxValue,
;                                     const uint32_t* args, uint32_t count)
;   Like fut_guest_call, but loads ecx/edx first: fastcall (ecx, edx, stack...) and thiscall (ecx = this).
fut_guest_call_reg PROC
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     ecx, [ebp+24]           ; count
    mov     esi, [ebp+20]           ; args
    test    ecx, ecx
    jz      load_regs
push_arg:
    push    dword ptr [esi+ecx*4-4]
    dec     ecx
    jnz     push_arg
load_regs:
    mov     ecx, [ebp+12]
    mov     edx, [ebp+16]
    mov     eax, [ebp+8]
    call    eax
    lea     esp, [ebp-12]
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret
fut_guest_call_reg ENDP

END
