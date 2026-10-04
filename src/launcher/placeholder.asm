; XBE address-space placeholder (design 1.3 item 2, launcher_probe.md).
; A code contribution named .text$a: the linker merges it into .text and orders it before the
; compiler's .text$mn, so with /BASE:0x10000 it starts exactly at 0x11000 (first section after the
; header page). Size 0x30A000 -> 0x11000..0x31B000 covers all XBE sections (end 0x31A2F8).
; The launcher checks _xbe_space == 0x11000 and _xbe_space_end - _xbe_space at startup.
.686
.model flat
xbespace SEGMENT ALIAS(".text$a") PAGE PUBLIC READ EXECUTE 'CODE'
PUBLIC _xbe_space
PUBLIC _xbe_space_end
_xbe_space LABEL BYTE
    BYTE 30A000h DUP (0CCh)
_xbe_space_end LABEL BYTE
xbespace ENDS
END
