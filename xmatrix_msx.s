; xmatrix_msx.s — hardware helpers for xmatrix screensaver (MSX VDP + CPC banking)
; SCC cdecl convention: args on stack, callee preserves IX, IY, BC.
;
; MSX2 V9938 VDP ports:
;   0x98 — VRAM data read/write
;   0x99 — control: address setup, register write
;
; VRAM address setup (write mode):
;   di
;   out (0x99), addr_lo               ; A7:A0
;   out (0x99), (addr_hi & 0x3F)|0x40 ; A13:A8, bits7:6 = 01 = write
;   ei
;
; R#14 holds address bits A16:A14 for addresses >= 0x4000.
; Register write: out(0x99, value); out(0x99, 0x80 | regnum).
;
; CPC Bank_Copy kernel protocol (RST #0x20, .word #0x8130):
;   A  = (bankDst << 4) | bankSrc
;   DE = destAddr   (bank bankDst)
;   HL = srcAddr    (bank bankSrc)
;   BC = len

.z80
.code

; -----------------------------------------------------------------------
; void vdp_write(unsigned int vram_addr, char* src, unsigned short len)
;
; Writes len bytes from src to VDP VRAM starting at vram_addr.
; -----------------------------------------------------------------------
.export _vdp_write
_vdp_write:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)      ; E = vram_addr lo
	ld   d, (ix+5)      ; D = vram_addr hi
	ld   l, (ix+6)      ; HL = src
	ld   h, (ix+7)
	ld   c, (ix+8)      ; BC = len
	ld   b, (ix+9)

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
; -----------------------------------------------------------------------
.export _vdp_fill
_vdp_fill:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)      ; E = vram_addr lo
	ld   d, (ix+5)      ; D = vram_addr hi
	ld   l, (ix+6)      ; L = fill byte
	ld   c, (ix+8)      ; BC = len
	ld   b, (ix+9)

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


; -----------------------------------------------------------------------
; void vdp_write_char(unsigned int base_addr, char* src)
;
; Writes one 8×8 character cell (8 rows × 4 bytes = 32 bytes) to MSX VRAM.
; base_addr = VRAM address of the first row (= cy*2048 + cx*4).
; src       = 32-byte 4bpp nibble font data, 4 bytes per row.
;
; Sets R#14 once from base_addr[15:14].  Advances the row address by
; incrementing D (= +256 bytes per row, one Screen-7 scanline).
; Safe for all cells in the 64×26 grid: the 8-row span (7×256 = 1792
; bytes) never crosses a 16-KB R#14 boundary.
; -----------------------------------------------------------------------
.export _vdp_write_char
_vdp_write_char:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)      ; E = base_addr lo  (column offset, stays fixed)
	ld   d, (ix+5)      ; D = base_addr hi  (row counter, incremented per row)
	ld   l, (ix+6)      ; HL = src
	ld   h, (ix+7)

	; Set R#14 once (addr bits 15:14 = D >> 6 via two left-rotates + mask)
	di
	ld   a, d
	rlca
	rlca
	and  #3
	out  (0x99), a
	ld   a, #0x8E
	out  (0x99), a
	ei

	ld   b, #8
_vdp_wchar_row:
	; Set row address (E stays fixed; D advances each iteration)
	di
	ld   a, e
	out  (0x99), a
	ld   a, d
	and  #0x3F
	or   #0x40
	out  (0x99), a
	; Write 4 bytes
	ld   a, (hl)
	out  (0x98), a
	inc  hl
	ld   a, (hl)
	out  (0x98), a
	inc  hl
	ld   a, (hl)
	out  (0x98), a
	inc  hl
	ld   a, (hl)
	out  (0x98), a
	inc  hl
	ei
	inc  d              ; advance VRAM row by 256 bytes
	djnz _vdp_wchar_row

	pop  ix
	pop  bc
	ret


; -----------------------------------------------------------------------
; void vdp_erase_char(unsigned int base_addr)
;
; Fills one 8×8 character cell with ink1+ink1 (0x11 = background) in
; MSX VRAM.  Same addressing scheme as vdp_write_char.
; -----------------------------------------------------------------------
.export _vdp_erase_char
_vdp_erase_char:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)
	ld   d, (ix+5)

	di
	ld   a, d
	rlca
	rlca
	and  #3
	out  (0x99), a
	ld   a, #0x8E
	out  (0x99), a
	ei

	ld   b, #8
_vdp_erase_row:
	di
	ld   a, e
	out  (0x99), a
	ld   a, d
	and  #0x3F
	or   #0x40
	out  (0x99), a
	ld   a, #0x11
	out  (0x98), a
	out  (0x98), a
	out  (0x98), a
	out  (0x98), a
	ei
	inc  d
	djnz _vdp_erase_row

	pop  ix
	pop  bc
	ret


; -----------------------------------------------------------------------
; void cpc_write_char(unsigned short base_addr, char* src)
;
; Copies one 8×8 CPC character cell (8 scanlines × 2 bytes) to screen
; bank 0.  Scanlines are 0x800 bytes apart; base_addr is the address of
; scanline 0.  Uses RST #0x8130 (Bank_Copy kernel call) directly to
; avoid 8 separate C function call overheads.
;
; RST #0x8130 protocol: A=(dstBank<<4)|srcBank, DE=dst, HL=src, BC=len.
; dstBank=0 (screen), srcBank=__symbank (process data bank).
; -----------------------------------------------------------------------
.export _cpc_write_char
_cpc_write_char:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)      ; DE = base_addr (scanline 0, screen bank 0)
	ld   d, (ix+5)
	ld   l, (ix+6)      ; HL = src (font row 0 in data bank)
	ld   h, (ix+7)

	ld   b, #8
_cpc_wchar_row:
	push bc             ; save row counter
	push de             ; save dest scanline addr
	push hl             ; save src row addr
	ld   a, (__symbank) ; A = (0<<4) | srcBank
	ld   bc, #2
	rst  #0x20
	.word #0x8130
	pop  hl
	pop  de
	pop  bc

	inc  hl             ; src += 2 (next font row, 2 bytes each)
	inc  hl
	ld   a, d           ; dest += 0x800 (next CPC scanline)
	add  a, #8
	ld   d, a

	djnz _cpc_wchar_row

	pop  ix
	pop  bc
	ret


; -----------------------------------------------------------------------
; void cpc_erase_char(unsigned short base_addr)
;
; Fills one 8×8 CPC character cell with background (ink1 = 0xF0) across
; 8 scanlines, using the same Bank_Copy approach as cpc_write_char.
; -----------------------------------------------------------------------
.export _cpc_erase_char
_cpc_erase_char:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)
	ld   d, (ix+5)

	ld   b, #8
_cpc_erase_row:
	push bc
	push de
	ld   hl, #_cpc_bg_bytes ; fixed 2-byte bg source in data bank
	ld   a, (__symbank)
	ld   bc, #2
	rst  #0x20
	.word #0x8130
	pop  de
	pop  bc

	ld   a, d
	add  a, #8
	ld   d, a

	djnz _cpc_erase_row

	pop  ix
	pop  bc
	ret


; -----------------------------------------------------------------------
; Data: 2-byte CPC background fill (ink1 Mode 1 encoding = 0xF0 per byte)
; Must be in .symdata so it lives in the process data bank (_symbank).
; -----------------------------------------------------------------------
.symdata
_cpc_bg_bytes:
	.byte 0xF0
	.byte 0xF0
