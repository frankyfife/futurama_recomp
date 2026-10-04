; XBE address-space placeholder: a code contribution named .text$a, so the linker
; merges it into .text and orders it before the compiler's .text$mn.
.686
.model flat
xbespace SEGMENT ALIAS(".text$a") PAGE PUBLIC READ EXECUTE 'CODE'
PUBLIC _xbe_space
_xbe_space LABEL BYTE
    BYTE 30A000h DUP (0CCh)
xbespace ENDS
END
