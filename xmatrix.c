// xmatrix.c — Matrix screensaver for SymbOS (CPC and MSX)
//
// CPC Mode 1 — 320×200, 4 colours, 2bpp packed with bit-shuffled layout:
//   bit7=p0_lo  bit6=p1_lo  bit5=p2_lo  bit4=p3_lo
//   bit3=p0_hi  bit2=p1_hi  bit1=p2_hi  bit0=p3_hi
// SymbOS default Mode 1 palette: ink1=black, ink2=dim-green, ink3=bright-green.
//   bg    (ink1): 0xF0 per 4-pixel byte
//   dim   (ink2): 0x0F per 4-pixel byte
//   bright(ink3): 0xFF per 4-pixel byte
//   white (ink0): 0x00 per 4-pixel byte
// CPC scanlines are 0x800 bytes apart; row_base[cy] = 0xC000 + cy*80.
//
// MSX Screen 7 — 512×212, 16 colours, 4bpp nibble layout:
//   high nibble = left pixel, low nibble = right pixel
//   2 pixels per byte, 256 bytes per row (linear, no interleave).
// VRAM written via VDP ports 0x98 (data) / 0x99 (address).
// SymbOS colour indices (windows.h): 1=black, 8=white, 9=green,
//   10=lgreen, 13=gray — used for bg/dim/bright/glow/white.

#include <symbos.h>
#include <symbos/msgid.h>
#include <symbos/keys.h>
#include <stdlib.h>
#include <string.h>

#define MSC_SAV_INIT   1
#define MSC_SAV_START  2
#define MSC_SAV_CONFIG 3
#define MSR_SAV_CONFIG 4

// CPC screen geometry
#define SCREEN_W_CPC  320
#define SCREEN_H_CPC  200
#define GRID_W_CPC    40    /* 320 / 8 */
#define GRID_H_CPC    25    /* 200 / 8 */

// MSX Screen 7 geometry
#define SCREEN_W_MSX  512
#define SCREEN_H_MSX  212
#define GRID_W_MSX    64    /* 512 / 8 */
#define GRID_H_MSX    26    /* 212 / 8, last row partially visible */

#define NGLYPHS_BINARY 2
#define NGLYPHS_HANA   9
#define NGLYPHS_TOTAL  11
#define GLOW_MAX   4        /* glow frames on new char */
#define GLOW_WHITE 2        /* glow > GLOW_WHITE -> white  (2 frames: 4,3) */
#define GLOW_GLOW  1        /* glow > GLOW_GLOW  -> glow   (1 frame:  2)   */
                            /* glow > 0          -> bright (1 frame:  1)   */
                            /* glow == 0         -> dim    (permanent)     */

// MSX palette ink values (4-bit nibble) — SymbOS colour indices (windows.h)
#define MSX_BG     0x1      /* COLOR_BLACK  */
#define MSX_DIM    0x9      /* COLOR_GREEN  (dark green)  */
#define MSX_BRIGHT 0xA      /* COLOR_LGREEN (light green) */
#define MSX_GLOW   0xD      /* COLOR_GRAY   (cooling step before white) */
#define MSX_WHITE  0x8      /* COLOR_WHITE  */

// -----------------------------------------------------------------------
// 8x8 font source bitmaps: '.' = background, '#' = foreground pixel
// -----------------------------------------------------------------------

static const char *font_art[NGLYPHS_TOTAL][8] = {
    { /* binary: 0 */
        "..####..",
        ".##..##.",
        ".##.###.",
        ".###.##.",
        ".##..##.",
        ".##..##.",
        "..####..",
        "........",
    },
    { /* binary: 1 */
        "...##...",
        "..###...",
        "...##...",
        "...##...",
        "...##...",
        "...##...",
        ".######.",
        "........",
    },
    { /* hana: エ (e) */
        "########",
        "...##...",
        "...##...",
        "...##...",
        "...##...",
        "########",
        "........",
        "........",
    },
    { /* hana: コ (ko) */
        ".######.",
        ".##.....",
        ".##.....",
        ".##.....",
        ".##.....",
        ".######.",
        "........",
        "........",
    },
    { /* hana: ス (su) */
        ".######.",
        "....##..",
        "...##...",
        "..####..",
        ".##..##.",
        "##....##",
        "........",
        "........",
    },
    { /* hana: ト (to) */
        "....##..",
        "....##..",
        ".#######",
        "....##..",
        "....##..",
        "....##..",
        "........",
        "........",
    },
    { /* hana: ネ (ne) */
        "...##...",
        "########",
        "...##...",
        ".##..##.",
        "##....##",
        "........",
        "........",
        "........",
    },
    { /* hana: ク (ku) */
        ".######.",
        "......##",
        ".....##.",
        "....##..",
        "...##...",
        "..##....",
        "........",
        "........",
    },
    { /* hana: ロ (ro) */
        ".######.",
        ".##..##.",
        ".##..##.",
        ".##..##.",
        ".##..##.",
        ".######.",
        "........",
        "........",
    },
    { /* hana: モ (mo) */
        "..####..",
        "...##...",
        "########",
        "...##...",
        "...#####",
        "........",
        "........",
        "........",
    },
    { /* hana: ツ (tsu) */
        ".##.####",
        ".##..##.",
        "..##.##.",
        "....####",
        "....###.",
        "...##...",
        "........",
        "........",
    },
};

// CPC pre-encoded font: 16 bytes per glyph (8 rows × 2 bytes)
_data unsigned char font_white[NGLYPHS_TOTAL * 16];   /* fg=ink0, bg=ink1 */
_data unsigned char font_bright[NGLYPHS_TOTAL * 16];  /* fg=ink3, bg=ink1 */
_data unsigned char font_dim[NGLYPHS_TOTAL * 16];     /* fg=ink2, bg=ink1 */

// MSX pre-encoded font: 32 bytes per glyph (8 rows × 4 bytes, 4bpp nibble)
_data unsigned char font_white_msx[NGLYPHS_TOTAL * 32];   /* ink15 white */
_data unsigned char font_glow_msx[NGLYPHS_TOTAL * 32];    /* ink14 gray  */
_data unsigned char font_bright_msx[NGLYPHS_TOTAL * 32];  /* ink3  light green */
_data unsigned char font_dim_msx[NGLYPHS_TOTAL * 32];     /* ink2  medium green */

// CPC screen clear buffer: one scanline plane (25 rows × 80 bytes = 2000)
_data unsigned char zero_plane[2000];

// CPC 2-byte clear for erasing one character column-pair
_data unsigned char bg_row[2]; /* { 0xF0, 0xF0 } */

// -----------------------------------------------------------------------
// Grid state — sized for MSX (larger of the two grids)
// -----------------------------------------------------------------------

typedef struct {
    unsigned char glyph;   /* 1-based index (0 = empty/black) */
    unsigned char glow;    /* countdown: >0 = bright, 0 = dim  */
    unsigned char dirty;   /* needs redraw this frame           */
} Cell;

typedef struct {
    signed char   y;        /* current row (-1 = inactive)  */
    unsigned char remaining;/* chars left to place          */
    unsigned char throttle; /* delay ticks before moving    */
} Feeder;

Cell   cells[GRID_W_MSX * GRID_H_MSX];
Feeder feeders[GRID_W_MSX];

// CPC row base addresses (precomputed to avoid multiply in draw loop)
static unsigned short row_base[GRID_H_MSX];

// Platform flag: set at animation start via Sys_Type()
_transfer char is_msx = 0;

// VDP functions in xmatrix_msx.s
extern void vdp_write(unsigned int vram_addr, char* src, unsigned short len);
extern void vdp_fill(unsigned int vram_addr, unsigned char fill_byte, unsigned short len);
extern void vdp_write_char(unsigned int vram_addr, char* src);
extern void vdp_erase_char(unsigned int vram_addr);
extern void cpc_write_char(unsigned short base_addr, char* src);
extern void cpc_erase_char(unsigned short base_addr);

// -----------------------------------------------------------------------
// Font and buffer initialisation
// -----------------------------------------------------------------------

static void build_font(void)
{
    unsigned char g, r, b, hi, lo, lp, rp;
    const char *row;

    for (g = 0; g < NGLYPHS_TOTAL; g++) {
        for (r = 0; r < 8; r++) {
            row = font_art[g][r];

            // CPC Mode 1 encoding (2bpp, 4px per byte)
            hi = 0;
            if (row[0] == '#') hi |= 0x08;
            if (row[1] == '#') hi |= 0x04;
            if (row[2] == '#') hi |= 0x02;
            if (row[3] == '#') hi |= 0x01;
            lo = 0;
            if (row[4] == '#') lo |= 0x08;
            if (row[5] == '#') lo |= 0x04;
            if (row[6] == '#') lo |= 0x02;
            if (row[7] == '#') lo |= 0x01;

            font_white[g * 16 + r * 2 + 0]  = (unsigned char)((~hi & 0x0F) << 4);
            font_white[g * 16 + r * 2 + 1]  = (unsigned char)((~lo & 0x0F) << 4);

            font_bright[g * 16 + r * 2 + 0] = 0xF0 | hi;
            font_bright[g * 16 + r * 2 + 1] = 0xF0 | lo;

            font_dim[g * 16 + r * 2 + 0] = ((unsigned char)(~hi & 0x0F) << 4) | hi;
            font_dim[g * 16 + r * 2 + 1] = ((unsigned char)(~lo & 0x0F) << 4) | lo;

            // MSX 4bpp nibble encoding (2px per byte, high nibble = left px)
            for (b = 0; b < 4; b++) {
                lp = (row[b * 2]     == '#') ? 1 : 0;
                rp = (row[b * 2 + 1] == '#') ? 1 : 0;
                font_dim_msx[g * 32 + r * 4 + b] =
                    (lp ? MSX_DIM    : MSX_BG) << 4 | (rp ? MSX_DIM    : MSX_BG);
                font_bright_msx[g * 32 + r * 4 + b] =
                    (lp ? MSX_BRIGHT : MSX_BG) << 4 | (rp ? MSX_BRIGHT : MSX_BG);
                font_glow_msx[g * 32 + r * 4 + b] =
                    (lp ? MSX_GLOW   : MSX_BG) << 4 | (rp ? MSX_GLOW   : MSX_BG);
                font_white_msx[g * 32 + r * 4 + b] =
                    (lp ? MSX_WHITE  : MSX_BG) << 4 | (rp ? MSX_WHITE  : MSX_BG);
            }
        }
    }

    if (!is_msx) {
        for (r = 0; r < GRID_H_CPC; r++)
            row_base[r] = 0xC000u + (unsigned short)r * 80u;
        memset(zero_plane, 0xF0, sizeof(zero_plane));
        bg_row[0] = bg_row[1] = 0xF0;
    }
}

// -----------------------------------------------------------------------
// Key detection (hardware scan — works while desktop is stopped)
// -----------------------------------------------------------------------

static unsigned char any_key_down(void)
{
    unsigned char sc;
    for (sc = 0; sc < 80; sc++)
        if (Key_Down(sc)) return 1;
    return 0;
}

// -----------------------------------------------------------------------
// Screen operations
// -----------------------------------------------------------------------

static void mx_clear_screen(void)
{
    unsigned char k;
    if (is_msx) {
        // 212 rows × 256 bytes/row = 54272 bytes, all ink1 pairs (0x11)
        vdp_fill(0u, 0x11u, 54272u);
    } else {
        for (k = 0; k < 8; k++) {
            Bank_Copy(0,
                (char *)(0xC000u + (unsigned short)k * 0x0800u),
                _symbank, (char *)zero_plane, 2000u);
        }
    }
}

// Draw one 8×8 character cell at grid position (cx, cy).
static void draw_cell(unsigned char cx, unsigned char cy,
                      unsigned char gidx, unsigned char color)
{
    unsigned char *fnt;
    unsigned short base_cpc;
    unsigned int   base_msx;

    if (is_msx) {
        fnt = (color == 3 ? font_white_msx
             : color == 2 ? font_glow_msx
             : color == 1 ? font_bright_msx : font_dim_msx)
              + (unsigned int)gidx * 32u;
        base_msx = (unsigned int)cy * 2048u + (unsigned int)cx * 4u;
        vdp_write_char(base_msx, (char *)fnt);
    } else {
        fnt = (color >= 2 ? font_white : color == 1 ? font_bright : font_dim)
              + gidx * 16;
        base_cpc = row_base[cy] + (unsigned short)cx * 2u;
        cpc_write_char(base_cpc, (char *)fnt);
    }
}

static void erase_cell(unsigned char cx, unsigned char cy)
{
    unsigned short base_cpc;
    unsigned int   base_msx;

    if (is_msx) {
        base_msx = (unsigned int)cy * 2048u + (unsigned int)cx * 4u;
        vdp_erase_char(base_msx);
    } else {
        base_cpc = row_base[cy] + (unsigned short)cx * 2u;
        cpc_erase_char(base_cpc);
    }
}

// -----------------------------------------------------------------------
// Animation tick
// -----------------------------------------------------------------------

static void anim_tick(unsigned char density, unsigned char speed,
                      unsigned char glyph_base, unsigned char nglyphs,
                      unsigned char grid_w, unsigned char grid_h)
{
    unsigned int idx;
    unsigned int total_cells;
    unsigned char x;
    unsigned char y;
    Cell   *c;
    Feeder *f;
    unsigned char new_feeders;

    total_cells = (unsigned int)grid_w * grid_h;

    // --- Advance feeders ---
    for (x = 0; x < grid_w; x++) {
        f = &feeders[x];
        if (f->y < 0) continue;

        if (f->throttle > 0) {
            f->throttle--;
            continue;
        }

        if ((unsigned char)f->y < grid_h) {
            c = &cells[(unsigned char)f->y * grid_w + x];
            if (f->remaining > 0) {
                c->glyph = glyph_base + (rand() % nglyphs) + 1u;
                c->glow  = GLOW_MAX;
                c->dirty = 1;
                f->remaining--;
            } else {
                if (c->glyph) {
                    c->glyph = 0;
                    c->glow  = 0;
                    c->dirty = 1;
                }
            }
        }

        f->y++;
        if (f->y >= grid_h) {
            f->y       = -1;
            f->remaining = 0;
        }
    }

    // --- Glow decay ---
    c = cells;
    for (idx = 0; idx < total_cells; idx++, c++) {
        if (c->glyph && c->glow > 0) {
            c->glow--;
            if (c->glow == GLOW_WHITE || c->glow == GLOW_GLOW || c->glow == 0)
                c->dirty = 1;
        }
    }

    // --- Activate new feeders based on density setting ---
    new_feeders = (density < 2) ? 1 : (density < 3) ? 2 : 4;
    while (new_feeders--) {
        x = (unsigned char)(rand() % grid_w);
        f = &feeders[x];
        if (f->y >= 0) continue;
        f->y         = (signed char)(rand() % (grid_h / 3));
        f->remaining = 5 + (unsigned char)(rand() % (grid_h - 5));
        f->throttle  = (speed >= 3) ? 0 : (speed == 2) ? (unsigned char)(rand() % 4) : (unsigned char)(rand() % 8);
    }

    // --- Render dirty cells ---
    c = cells;
    for (y = 0; y < grid_h; y++) {
        for (x = 0; x < grid_w; x++, c++) {
            if (!c->dirty) continue;
            c->dirty = 0;
            if (!c->glyph) {
                erase_cell(x, y);
            } else {
                draw_cell(x, y, c->glyph - 1u,
                    c->glow > GLOW_WHITE ? 3 :
                    c->glow > GLOW_GLOW  ? 2 :
                    c->glow > 0          ? 1 : 0);
            }
        }
    }
}

// -----------------------------------------------------------------------
// Config data and dialog
// -----------------------------------------------------------------------

// cfgdat[0..3] = "MATX" magic; [4] = density (1-3); [5] = speed (1-3); [6] = glyphset (1=binary, 2=hana)
// Must be in _data (app bank), NOT _transfer: the desktop reads it via bank-switching, and the
// transfer segment is process-local — when the desktop is active, 0xC000 maps to *its own*
// transfer RAM, so a transfer-segment address would produce zeros in SYMBOS.INI.
_data char cfgdat[64];
_transfer char tmp_density         = 2;
_transfer char tmp_speed           = 2;
_transfer char tmp_glyphset        = 1;
_transfer char cfg_prz             = 0;
_transfer signed char cfgwin_id    = -1;

_transfer char rg_density[4]       = { -1, -1, -1, -1 };
_transfer char rg_speed[4]         = { -1, -1, -1, -1 };
_transfer char rg_glyphset[4]      = { -1, -1, -1, -1 };

_transfer Ctrl_TFrame cfg_tf       = { "Settings",  (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_d    = { "Density:",  (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_s    = { "Speed:",    (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_g    = { "Glyphs:",   (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };

_transfer Ctrl_Radio cfg_rad_d1    = { &tmp_density,  "Sparse", (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_density  };
_transfer Ctrl_Radio cfg_rad_d2    = { &tmp_density,  "Normal", (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_density  };
_transfer Ctrl_Radio cfg_rad_d3    = { &tmp_density,  "Dense",  (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_density  };
_transfer Ctrl_Radio cfg_rad_s1    = { &tmp_speed,    "Slow",   (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_speed    };
_transfer Ctrl_Radio cfg_rad_s2    = { &tmp_speed,    "Normal", (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_speed    };
_transfer Ctrl_Radio cfg_rad_s3    = { &tmp_speed,    "Fast",   (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_speed    };
_transfer Ctrl_Radio cfg_rad_g1    = { &tmp_glyphset, "Binary", (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_glyphset };
_transfer Ctrl_Radio cfg_rad_g2    = { &tmp_glyphset, "Kana",   (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_glyphset };

_transfer Ctrl ccc0  = { 0,  C_AREA,   -1, COLOR_ORANGE,                    0,  0, 208, 74, 0 };
_transfer Ctrl ccc1  = { 0,  C_TFRAME, -1, (unsigned short)&cfg_tf,         2,  1, 204, 50, 0 };
_transfer Ctrl ccc2  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_d,      8, 10,  44,  8, 0 };
_transfer Ctrl ccc3  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d1,    56, 10,  38,  8, 0 };
_transfer Ctrl ccc4  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d2,    96, 10,  38,  8, 0 };
_transfer Ctrl ccc5  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d3,   136, 10,  34,  8, 0 };
_transfer Ctrl ccc6  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_s,      8, 22,  44,  8, 0 };
_transfer Ctrl ccc7  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s1,    56, 22,  30,  8, 0 };
_transfer Ctrl ccc8  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s2,    88, 22,  38,  8, 0 };
_transfer Ctrl ccc9  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s3,   128, 22,  30,  8, 0 };
_transfer Ctrl ccc10 = { 10, C_BUTTON, -1, (unsigned short)"OK",            54, 58,  32, 12, 0 };
_transfer Ctrl ccc11 = { 11, C_BUTTON, -1, (unsigned short)"Cancel",        94, 58,  52, 12, 0 };
_transfer Ctrl ccc12 = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_g,      8, 34,  44,  8, 0 };
_transfer Ctrl ccc13 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_g1,    56, 34,  46,  8, 0 };
_transfer Ctrl ccc14 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_g2,   106, 34,  34,  8, 0 };

_transfer Ctrl_Group cfgcg;
_transfer Window     cfgwin;
_transfer char       cfg_title[7] = { 'M', 'a', 't', 'r', 'i', 'x', 0 };
_data     char       init_tmp[64]; /* must be _data, not _transfer: Bank_Copy dest must be in app bank */

// Animation window (minimal fullscreen)
_transfer Ctrl       anim_ctrl[1];
_transfer Ctrl_Group anim_cg;
_transfer Window     anim_win;
_transfer char       empty_str[1];

// -----------------------------------------------------------------------
// Config helpers
// -----------------------------------------------------------------------

static void cfg_open(void)
{
    if (cfgwin_id >= 0) return;

    tmp_density   = cfgdat[4];
    tmp_speed     = cfgdat[5];
    tmp_glyphset  = cfgdat[6];

    rg_density[0]  = rg_density[1]  = rg_density[2]  = rg_density[3]  = -1;
    rg_speed[0]    = rg_speed[1]    = rg_speed[2]    = rg_speed[3]    = -1;
    rg_glyphset[0] = rg_glyphset[1] = rg_glyphset[2] = rg_glyphset[3] = -1;

    memset(&cfgcg, 0, sizeof(cfgcg));
    cfgcg.controls = 15;
    cfgcg.pid      = _sympid;
    cfgcg.first    = &ccc0;

    memset(&cfgwin, 0, sizeof(cfgwin));
    cfgwin.state    = WIN_NORMAL;
    cfgwin.flags    = WIN_TITLE | WIN_CENTERED | WIN_NOTTASKBAR;
    cfgwin.pid      = _sympid;
    cfgwin.w        = 208;
    cfgwin.h        = 74;
    cfgwin.wfull    = 208;
    cfgwin.hfull    = 74;
    cfgwin.wmin     = 208;
    cfgwin.hmin     = 74;
    cfgwin.wmax     = 208;
    cfgwin.hmax     = 74;
    cfgwin.title    = cfg_title;
    cfgwin.controls = &cfgcg;

    cfgwin_id = Win_Open(_symbank, &cfgwin);
}

static void cfg_close(void)
{
    if (cfgwin_id < 0) return;
    Win_Close((unsigned char)cfgwin_id);
    cfgwin_id = -1;
}

static void cfg_ok(void)
{
    cfgdat[4] = tmp_density;
    cfgdat[5] = tmp_speed;
    cfgdat[6] = tmp_glyphset;
    cfg_close();

    if (cfg_prz) {
        _symmsg[0] = MSR_SAV_CONFIG;
        _symmsg[1] = _symbank;
        _symmsg[2] = (char)((unsigned short)cfgdat & 0xFF);
        _symmsg[3] = (char)((unsigned short)cfgdat >> 8);
        while (!Msg_Send(_sympid, cfg_prz, _symmsg));
        cfg_prz = 0;
    }
}

static void cfg_cancel(void)
{
    cfg_close();
    cfg_prz = 0;
}

// -----------------------------------------------------------------------
// Desktop stop / resume
// -----------------------------------------------------------------------

static void desktop_stop(unsigned char wid)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKSTP;
    _symmsg[2] = 0xFF;
    _symmsg[3] = wid;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Msg_Wait(_sympid, 2, _symmsg, MSR_DSK_DSKSRV);
}

static void desktop_cont(void)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKCNT;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Idle();
}

// -----------------------------------------------------------------------
// Animation entry point
// -----------------------------------------------------------------------

void start_animation(void)
{
    signed char   wid;
    unsigned char tick, density, speed, idle_skip, burst, b;
    unsigned char glyph_set, glyph_base, nglyphs;
    unsigned char grid_w, grid_h;
    unsigned short screen_w, screen_h;
    unsigned short mx0, my0;
    unsigned short resp;
    unsigned int  i;

    density   = (unsigned char)cfgdat[4];
    speed     = (unsigned char)cfgdat[5];
    glyph_set = (unsigned char)cfgdat[6];

    if (density   < 1 || density   > 3) density   = 2;
    if (speed     < 1 || speed     > 3) speed     = 2;
    if (glyph_set < 1 || glyph_set > 2) glyph_set = 1;

    if (glyph_set == 2) {
        glyph_base = NGLYPHS_BINARY;
        nglyphs    = NGLYPHS_HANA;
    } else {
        glyph_base = 0;
        nglyphs    = NGLYPHS_BINARY;
    }

    idle_skip = (speed == 1) ? 6 : 1;
    burst     = (speed == 3) ? 3 : 1;

    // Detect platform
    is_msx = ((Sys_Type() & TYPE_MSX) != 0) ? 1 : 0;

    if (is_msx) {
        grid_w   = GRID_W_MSX;
        grid_h   = GRID_H_MSX;
        screen_w = SCREEN_W_MSX;
        screen_h = SCREEN_H_MSX;
    } else {
        grid_w   = GRID_W_CPC;
        grid_h   = GRID_H_CPC;
        screen_w = SCREEN_W_CPC;
        screen_h = SCREEN_H_CPC;
    }

    srand((unsigned int)Sys_Counter());

    build_font();

    // Reset grid (only the portion used by this platform)
    memset(cells,   0, (unsigned int)grid_w * grid_h * sizeof(Cell));
    memset(feeders, 0, grid_w * sizeof(Feeder));
    for (i = 0; i < grid_w; i++)
        feeders[i].y = -1;

    // Open fullscreen animation window
    empty_str[0] = 0;

    anim_ctrl[0].value  = 0;
    anim_ctrl[0].type   = C_AREA;
    anim_ctrl[0].bank   = -1;
    anim_ctrl[0].param  = AREA_16COLOR | COLOR_BLACK;
    anim_ctrl[0].x      = 0;
    anim_ctrl[0].y      = 0;
    anim_ctrl[0].w      = screen_w;
    anim_ctrl[0].h      = screen_h;
    anim_ctrl[0].unused = 0;

    memset(&anim_cg, 0, sizeof(anim_cg));
    anim_cg.controls = 1;
    anim_cg.pid      = _sympid;
    anim_cg.first    = &anim_ctrl[0];

    memset(&anim_win, 0, sizeof(anim_win));
    anim_win.state    = WIN_NORMAL;
    anim_win.flags    = WIN_NOTTASKBAR | WIN_NOTMOVEABLE;
    anim_win.pid      = _sympid;
    anim_win.w        = screen_w;
    anim_win.h        = screen_h;
    anim_win.wfull    = screen_w;
    anim_win.hfull    = screen_h;
    anim_win.wmin     = 32;
    anim_win.hmin     = 24;
    anim_win.wmax     = screen_w;
    anim_win.hmax     = screen_h;
    anim_win.title    = empty_str;
    anim_win.status   = empty_str;
    anim_win.controls = &anim_cg;

    wid = Win_Open(_symbank, &anim_win);
    if (wid < 0) return;

    desktop_stop((unsigned char)wid);
    mx_clear_screen();

    mx0  = Mouse_X();
    my0  = Mouse_Y();
    tick = 0;

    while (1) {
        if (Mouse_X()     != mx0 ||
            Mouse_Y()     != my0 ||
            Mouse_Buttons()       ||
            any_key_down()) {

            desktop_cont();
            Idle();
            Win_Close((unsigned char)wid);
            Screen_Redraw();
            return;
        }

        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 1) {
            switch (_symmsg[0]) {
            case 0:
                desktop_cont();
                Win_Close((unsigned char)wid);
                exit(0);
            case MSR_DSK_WFOCUS:
                break;
            default:
                break;
            }
        }

        if (++tick >= idle_skip) {
            tick = 0;
            for (b = 0; b < burst; b++)
                anim_tick(density, speed, glyph_base, nglyphs, grid_w, grid_h);
        }

        Idle();
    }
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------

int main(int argc, char *argv[])
{
    unsigned short resp;
    unsigned char  got_msg, sender, b;

    /* _data vars are not statically initialised by SCC — set defaults explicitly */
    cfgdat[0] = 'M'; cfgdat[1] = 'A'; cfgdat[2] = 'T'; cfgdat[3] = 'X';
    cfgdat[4] = 2;   cfgdat[5] = 2;   cfgdat[6] = 1;

    got_msg = 0;
    sender  = 0;

    for (b = 0; b < 10; b++) {
        Idle();
        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 0x01) {
            got_msg = 1;
            sender  = (unsigned char)(resp >> 8);
            break;
        }
    }

    if (!got_msg) {
        start_animation();
        exit(0);
    }

    while (1) {
        switch (_symmsg[0]) {

        case 0:
            exit(0);

        case MSC_SAV_INIT:
            Bank_Copy(
                _symbank, init_tmp,
                (unsigned char)_symmsg[1],
                (char *)((unsigned short)((unsigned char)_symmsg[3] << 8)
                         | (unsigned char)_symmsg[2]),
                64u);
            if (init_tmp[0] == 'M' && init_tmp[1] == 'A' &&
                init_tmp[2] == 'T' && init_tmp[3] == 'X') {
                memcpy(cfgdat, init_tmp, 64);
            }
            break;

        case MSC_SAV_START:
            start_animation();
            break;

        case MSC_SAV_CONFIG:
            cfg_prz = sender;
            cfg_open();
            break;

        default:
            if ((unsigned char)_symmsg[0] == MSR_DSK_WCLICK &&
                cfgwin_id >= 0 &&
                (unsigned char)_symmsg[1] == (unsigned char)cfgwin_id) {

                if ((unsigned char)_symmsg[2] == DSK_ACT_CLOSE) {
                    cfg_cancel();
                } else if ((unsigned char)_symmsg[2] == DSK_ACT_CONTENT) {
                    if ((unsigned char)_symmsg[8] == 10)
                        cfg_ok();
                    else if ((unsigned char)_symmsg[8] == 11)
                        cfg_cancel();
                }
            }
            break;
        }

        do {
            resp = Msg_Sleep(_sympid, -1, _symmsg);
        } while (!(resp & 0x01));

        sender = (unsigned char)(resp >> 8);
    }
}
