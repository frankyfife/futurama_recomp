; Fiber switch replacing XAPILIB::SwitchToFiber 0x17094F (design 1.7 "Fibers", E9, R2; m1_report 9
; item 4) and a guest-SEH3 frame helper for the M2 self-tests. Owned by the fiber agent.
; C declarations: src/host/xapi/xapi_fiber.h.
;
; Original (63 bytes, disassembly 0x17094F..0x17098B, startup.md 4.2), stdcall 1 argument:
;   mov  edx,[_tls_index]          mov  ecx,fs:[4] (fs patch -> S_TLS)     mov  eax,[esp+4]
;   push ebp / esi / edi / ebx     push fs:[0]
;   mov  edx,[ecx+edx*4]  ; TLS block       mov  ecx,[edx+0Ch]  ; current fiber
;   mov  [ecx+0Ch],esp    ; current->savedEsp
;   mov  esp,[eax+0Ch]    ; new->savedEsp   mov  [edx+0Ch],eax  ; TLS->currentFiber = new
;   pop  fs:[0] / ebx / edi / esi / ebp     ret 4
; The replacement keeps that saved-context layout (fs:[0], ebx, edi, esi, ebp, return address), so
; the initial frame of the original CreateFiber 0x1708B0 (0x170902..0x170934: -1, 0, 0, 0, 0,
; 0x17086B) and fibers suspended by either version stay compatible. Host additions:
;  - fut_fiber_switch_prepare (C++, xapi/xapi_fiber.cpp) does the guest stores of the original
;    ([current+0Ch] = saved ESP, [TLS+0Ch] = new fiber, same reads of _tls_index and S_TLS) and returns
;    the new ESP plus TEB.NtTib.StackBase/StackLimit and TEB.DeallocationStack for the new fiber;
;    for a fresh fiber it replaces the saved ExceptionList -1 by the fiber-top SEH record (SEHOP).
;  - this code then switches ESP, writes the three TEB fields (fs:[4], fs:[8], fs:[0E0Ch]) and
;    restores the new fiber's context. No exception can occur between the ESP switch and the TEB
;    writes (only fs: stores), so Windows never sees ESP outside the TEB stack range.
.686
.model flat, C
OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE
ASSUME FS:NOTHING

fut_fiber_switch_prepare PROTO C :DWORD, :DWORD, :DWORD

; xapi::detail::FiberSwitchTarget (static_assert'ed size 16)
TARGET_ESP          EQU 0
TARGET_STACKBASE    EQU 4
TARGET_STACKLIMIT   EQU 8
TARGET_DEALLOC      EQU 12
TARGET_SIZE         EQU 16
; 32-bit TEB offsets (NT_TIB.StackBase, NT_TIB.StackLimit, TEB.DeallocationStack)
TEB_STACKBASE       EQU 4
TEB_STACKLIMIT      EQU 8
TEB_DEALLOCSTACK    EQU 0E0Ch

.code

; void __stdcall fut_switch_to_fiber(uint32_t fiber)   (symbol _fut_switch_to_fiber@4, ret 4)
fut_switch_to_fiber PROC STDCALL, fiber:DWORD
    mov     eax, [esp+4]                    ; new fiber (lpFiber)
    push    ebp
    push    esi
    push    edi
    push    ebx
    push    dword ptr fs:[0]                ; saved context complete: esp = current fiber's saved ESP
    mov     ecx, esp
    sub     esp, TARGET_SIZE
    mov     ebx, esp                        ; ebx = FiberSwitchTarget (callee-saved across the call)
    push    ebx
    push    ecx
    push    eax
    call    fut_fiber_switch_prepare        ; cdecl (fiber, savedEsp, &target)
    add     esp, 12
    mov     eax, [ebx+TARGET_STACKBASE]
    mov     ecx, [ebx+TARGET_STACKLIMIT]
    mov     edx, [ebx+TARGET_DEALLOC]
    mov     esp, [ebx+TARGET_ESP]           ; switch to the new fiber's stack
    mov     dword ptr fs:[TEB_STACKBASE], eax
    mov     dword ptr fs:[TEB_STACKLIMIT], ecx
    mov     dword ptr fs:[TEB_DEALLOCSTACK], edx
    pop     dword ptr fs:[0]
    pop     ebx
    pop     edi
    pop     esi
    pop     ebp
    ret     4
fut_switch_to_fiber ENDP

; uint32_t __cdecl fut_seh3_frame_call(uint32_t sehProlog, uint32_t sehEpilog, uint32_t fn, uint32_t arg)
;
; A function body as MSVC 7 emits it for `__try { fn(arg); } __except(EXCEPTION_EXECUTE_HANDLER) {}`
; (compare IsBadReadPtr 0x1709C9..0x170A2C): `push 8; push scopetable; call __SEH_prolog` builds the
; frame (record {Next, _except_handler3 0x168DCC, scopetable, trylevel} at ebp-10h, saved esp at
; ebp-18h, exception pointers at ebp-14h, ebx/esi/edi saved); trylevel 0 around the call; the
; filter returns 1; the __except block reloads esp from [ebp-18h]; __SEH_epilog unlinks the record and
; restores ebx/esi/edi/ebp. The guest's _except_handler3 then performs the real guest unwind
; (__global_unwind2 -> RtlUnwind through kernel slot 105, __local_unwind2) before jumping to the
; __except block. Used by the self-test "guest SEH through a host wrapper frame" (selftest_m2.cpp).
fut_seh3_frame_call PROC
    push    8                               ; locals: [ebp-18h] saved esp, [ebp-14h] exception pointers
    push    OFFSET seh3_scope_table
    mov     eax, [esp+12]                   ; sehProlog (1st argument)
    call    eax                             ; ebp = frame; [ebp+8] = sehProlog .. [ebp+14h] = arg
    and     dword ptr [ebp-4], 0            ; __try (trylevel 0)
    push    dword ptr [ebp+14h]
    call    dword ptr [ebp+10h]             ; fn(arg)
    mov     esp, [ebp-18h]                  ; cdecl or stdcall callee
    or      dword ptr [ebp-4], -1           ; end of __try
    xor     eax, eax                        ; 0 = fn returned
    jmp     seh3_epilog
seh3_filter::                               ; filter (runs with ebp = frame): EXCEPTION_EXECUTE_HANDLER
    xor     eax, eax
    inc     eax
    ret
seh3_handler::                              ; __except block, entered after the guest unwind
    mov     esp, [ebp-18h]
    or      dword ptr [ebp-4], -1
    mov     eax, 1                          ; 1 = exception handled here
seh3_epilog:
    call    dword ptr [ebp+0Ch]             ; __SEH_epilog: fs:[0] = Next, pop edi/esi/ebx, leave
    ret
fut_seh3_frame_call ENDP

.const
; Scope table of fut_seh3_frame_call: one entry {enclosing level -1, filter, handler}.
seh3_scope_table DD -1, OFFSET seh3_filter, OFFSET seh3_handler

END
