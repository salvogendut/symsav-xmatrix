![Example Image](symbos-xmatrix.png)

# symsav-xmatrix

A Matrix-style falling character screensaver for [SymbOS](https://www.symbos.org/) on the Amstrad CPC.

> **Requires Mode 1** — this screensaver only works in 320×200 Mode 1 (4 colours). Running it in any other screen mode will produce incorrect output.

Inspired by Jamie Zawinski's [xmatrix](https://www.jwz.org/xscreensaver/) from the xscreensaver suite.

---

## Building

```bash
./build.sh
```

Requires the SCC compiler (set `SCC=` env var if not at `../scc/bin/cc`) and Python 3.

Build steps:

1. SCC compiles `xmatrix.c` → `xmatrix.sav`
2. `add_preview.py` patches the preview thumbnail into the binary at file offset 256

Output: `xmatrix.sav`

---

## Effect

- 40×25 grid of 8×8 pixel characters on the 320×200 CPC Mode 1 screen
- Columns of random characters fall from the top; each new character glows bright, then fades to dim
- Uses SymbOS default palette: bright (ink 3) for the leading character, dim (ink 2) for the trail, black (ink 1) for the background
- 2-glyph character set: binary digits `0` and `1`

Glyphs are defined as ASCII art strings in `xmatrix.c` (same convention as [symsav-xroach](https://github.com/salvogendut/symsav-xroach) sprites) and encoded into flat Mode 1 byte arrays at startup by `build_font()`.

---

## Screensaver protocol

Standard SymbOS screensaver messages:

| Message | Action |
|---------|--------|
| `MSC_SAV_INIT` (1) | Load saved config from manager |
| `MSC_SAV_START` (2) | Start fullscreen animation |
| `MSC_SAV_CONFIG` (3) | Open config dialog |
| `MSR_SAV_CONFIG` (4) | Send updated config back |

Config is 6 bytes: magic `"MATX"` + density byte + speed byte.

---

## Configuration

| Setting | Options |
|---------|---------|
| Density | Sparse / Normal / Dense — controls how many column streams activate per frame |
| Speed | Slow / Normal / Fast — controls the frame skip (6 / 3 / 1 idle ticks between frames) |

---

## Animation

Fullscreen rendering follows the same approach as [symsav-xroach](https://github.com/salvogendut/symsav-xroach):

1. Open a fullscreen `WIN_NOTTASKBAR | WIN_NOTMOVEABLE` window
2. `DSK_SRV_DSKSTP` to freeze the desktop
3. Clear all 8 CPC character planes via `Bank_Copy` to VRAM (bank 0)
4. Per frame: advance column feeders, decay glow, redraw only dirty cells
5. Exit on any key or mouse movement: resume desktop, close window, `Screen_Redraw()`

Screen address formula for character cell (cx, cy), scanline row r:

```
addr = 0xC000 + cy*80 + cx*2 + r*0x800
```
