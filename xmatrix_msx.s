; xmatrix_msx.s — MSX VDP helpers for xmatrix screensaver
; SCC cdecl convention: args on stack, callee preserves IX, IY, BC.
;
; MSX2 V9938 VDP ports:
;   0x98 — VRAM data read/write
;   0x99 — control: address setup, register write
;
; VRAM address setup (write mode):
;   di
;   out (#0x99), addr_lo               ; A7:A0
;   out (#0x99), (addr_hi & 0x3F)|0x40 ; A13:A8, bits7:6 = 01 = write
;   ei
;
; For VRAM addresses >= 0x4000 (Screen 7 needs up to 0xD3FF),
; register R#14 holds address bits A16:A14.  We set it before
; every call so we don't depend on any prior state.
;
; R#14 = addr[15:14] (A16=0 for all Screen-7 addresses we use).
; Register write: out(0x99, value); out(0x99, 0x80 | regnum).

.z80
.code

; -----------------------------------------------------------------------
; void vdp_write(unsigned int vram_addr, char* src, unsigned short len)
;
; Writes len bytes from src to VDP VRAM starting at vram_addr.
; Stack on entry: [ret(2), vram_addr(2), src(2), len(2)]
;
; After push bc / push ix:
;   ix = sp+2
;   ix+4 = vram_addr_lo
;   ix+5 = vram_addr_hi
;   ix+6 = src_lo
;   ix+7 = src_hi
;   ix+8 = len_lo
;   ix+9 = len_hi
; -----------------------------------------------------------------------
.export _vdp_write
_vdp_write:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	; Load all args into working registers before any VDP access
	ld   e, (ix+4)      ; E = vram_addr lo
	ld   d, (ix+5)      ; D = vram_addr hi
	ld   l, (ix+6)      ; HL = src
	ld   h, (ix+7)
	ld   c, (ix+8)      ; BC = len
	ld   b, (ix+9)

	; Set R#14 = addr[15:14] (= D >> 6)
	di
	ld   a, d
	rlca
	rlca
	and  #3
	out  (0x99), a      ; R#14 value
	ld   a, #0x8E       ; 0x80 | 14 = register-write command for R#14
	out  (0x99), a
	; Set VRAM write address
	ld   a, e           ; A7:A0
	out  (0x99), a
	ld   a, d
	and  #0x3F          ; mask off bits [7:6]
	or   #0x40          ; write mode: bit6=1, bit7=0
	out  (0x99), a
	ei

	; Write len bytes from (HL) to VDP data port
	ld   a, b
	or   c
	jr   z, _vdp_write_done
_vdp_write_loop:
	ld   a, (hl)
	out  (0x98), a
	inc  hl
	dec  bc
	ld   a, b
	or   c
	jr   nz, _vdp_write_loop
_vdp_write_done:
	pop  ix
	pop  bc
	ret


; -----------------------------------------------------------------------
; void vdp_fill(unsigned int vram_addr, unsigned char fill_byte,
;               unsigned short len)
;
; Fills len bytes of VDP VRAM starting at vram_addr with fill_byte.
; Stack on entry: [ret(2), vram_addr(2), fill_byte(2), len(2)]
;   ix+4 = vram_addr_lo
;   ix+5 = vram_addr_hi
;   ix+6 = fill_byte (1 byte, pushed as 2)
;   ix+8 = len_lo
;   ix+9 = len_hi
; -----------------------------------------------------------------------
.export _vdp_fill
_vdp_fill:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)      ; E = vram_addr lo
	ld   d, (ix+5)      ; D = vram_addr hi
	ld   l, (ix+6)      ; L = fill byte (safe register, no VDP overlap)
	ld   c, (ix+8)      ; BC = len
	ld   b, (ix+9)

	; Set R#14 and VRAM write address
	di
	ld   a, d
	rlca
	rlca
	and  #3
	out  (0x99), a
	ld   a, #0x8E
	out  (0x99), a
	ld   a, e
	out  (0x99), a
	ld   a, d
	and  #0x3F
	or   #0x40
	out  (0x99), a
	ei

	; Fill loop
	ld   a, b
	or   c
	jr   z, _vdp_fill_done
_vdp_fill_loop:
	ld   a, l
	out  (0x98), a
	dec  bc
	ld   a, b
	or   c
	jr   nz, _vdp_fill_loop
_vdp_fill_done:
	pop  ix
	pop  bc
	ret
