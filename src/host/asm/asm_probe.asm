; MASM sources of the host DLL (design 3.3: no inline assembly; stack switches and host->guest calls
; go here, e.g. guest_call.asm, fiber_switch.asm, adopt.asm). Owned by the core agent.
; This file only proves that MASM objects link into futurama_host.dll; it may be removed once real
; .asm files exist.
.686
.model flat, C
.code

; uint32_t __cdecl fut_asm_probe(void) - returns 'FUTA'
fut_asm_probe PROC
    mov eax, 46555441h
    ret
fut_asm_probe ENDP

END
