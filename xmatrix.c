// xmatrix.c — Matrix screensaver for SymbOS
// Falling-character effect modelled after Jamie Zawinski's xmatrix.
// Renders directly to CPC Mode 1 VRAM via Bank_Copy; uses SymbOS default palette.
//
// CPC Mode 1 byte layout for 4 pixels p0..p3 (each ink 0-3):
//   bit7=p0_lo  bit6=p1_lo  bit5=p2_lo  bit4=p3_lo
//   bit3=p0_hi  bit2=p1_hi  bit1=p2_hi  bit0=p3_hi
//
// SymbOS default Mode 1 palette: ink1 = black (background).
//   white (ink0): lo=0, hi=0  ->  0x00 per 4-pixel byte
//   bg    (ink1): lo=1, hi=0  ->  0xF0 per 4-pixel byte
//   dim   (ink2): lo=0, hi=1  ->  0x0F per 4-pixel byte
//   bright(ink3): lo=1, hi=1  ->  0xFF per 4-pixel byte
//
// Font encoding (nibble = 4-pixel foreground mask):
//   white  byte: (~nibble&0x0F)<<4        (bg=ink1, fg=ink0)
//   bright byte: 0xF0 | nibble            (bg=ink1, fg=ink3)
//   dim    byte: ((~nibble&0x0F)<<4)|nibble  (bg=ink1, fg=ink2)

#include <symbos.h>
#include <symbos/msgid.h>
#include <symbos/keys.h>
#include <stdlib.h>
#include <string.h>

#define MSC_SAV_INIT   1
#define MSC_SAV_START  2
#define MSC_SAV_CONFIG 3
#define MSR_SAV_CONFIG 4

#define SCREEN_W  320
#define SCREEN_H  200
#define GRID_W    40        /* 320 / 8 */
#define GRID_H    25        /* 200 / 8 */
#define NGLYPHS_BINARY 2
#define NGLYPHS_HANA   9
#define NGLYPHS_TOTAL  11
#define GLOW_MAX   6        /* total glow frames on new char */
#define GLOW_WHITE 3        /* glow > GLOW_WHITE -> white, else bright */

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

// Pre-encoded Mode 1 font: flat arrays, 16 bytes per glyph (8 rows * 2 bytes)
_data unsigned char font_white[NGLYPHS_TOTAL * 16];  /* fg=ink0, bg=ink1 */
_data unsigned char font_bright[NGLYPHS_TOTAL * 16]; /* fg=ink3, bg=ink1 */
_data unsigned char font_dim[NGLYPHS_TOTAL * 16];    /* fg=ink2, bg=ink1 */

// Screen clear buffer: one CPC character plane (25 rows * 80 bytes = 2000)
// filled with 0xF0 = ink1 (black) for all 4 pixels per byte
_data unsigned char zero_plane[2000];

// 2-byte clear for erasing one character column-pair
_data unsigned char bg_row[2]; /* { 0xF0, 0xF0 } */

// -----------------------------------------------------------------------
// Grid state
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

Cell   cells[GRID_W * GRID_H];
Feeder feeders[GRID_W];

// Row base addresses: 0xC000 + cy*80  (precomputed to avoid multiply in loop)
static unsigned short row_base[GRID_H];

// -----------------------------------------------------------------------
// Font and buffer initialisation
// -----------------------------------------------------------------------

static void build_font(void)
{
    unsigned char g, r, hi, lo;
    const char *row;

    for (g = 0; g < NGLYPHS_TOTAL; g++) {
        for (r = 0; r < 8; r++) {
            row = font_art[g][r];
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
        }
    }

    for (r = 0; r < GRID_H; r++)
        row_base[r] = 0xC000u + (unsigned short)r * 80u;

    memset(zero_plane, 0xF0, sizeof(zero_plane));
    bg_row[0] = bg_row[1] = 0xF0;
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
    for (k = 0; k < 8; k++) {
        Bank_Copy(0,
            (char *)(0xC000u + (unsigned short)k * 0x0800u),
            _symbank, (char *)zero_plane, 2000u);
    }
}

// Draw or erase one 8x8 character cell at grid position (cx, cy).
// Each of the 8 scanlines is 0x800 bytes apart in VRAM.
static void draw_cell(unsigned char cx, unsigned char cy,
                      unsigned char gidx, unsigned char color)
{
    unsigned char *fnt;
    unsigned short base;
    unsigned char r;

    fnt  = (color == 2 ? font_white : color == 1 ? font_bright : font_dim) + gidx * 16;
    base = row_base[cy] + (unsigned short)cx * 2u;

    for (r = 0; r < 8; r++) {
        Bank_Copy(0,
            (char *)(base + (unsigned short)r * 0x0800u),
            _symbank, (char *)(fnt + r * 2), 2u);
    }
}

static void erase_cell(unsigned char cx, unsigned char cy)
{
    unsigned short base;
    unsigned char r;

    base = row_base[cy] + (unsigned short)cx * 2u;

    for (r = 0; r < 8; r++) {
        Bank_Copy(0,
            (char *)(base + (unsigned short)r * 0x0800u),
            _symbank, (char *)bg_row, 2u);
    }
}

// -----------------------------------------------------------------------
// Animation tick
// -----------------------------------------------------------------------

static void anim_tick(unsigned char density, unsigned char speed,
                      unsigned char glyph_base, unsigned char nglyphs)
{
    unsigned int idx;
    unsigned char x;
    unsigned char y;
    Cell   *c;
    Feeder *f;
    unsigned char new_feeders;

    // --- Advance feeders ---
    for (x = 0; x < GRID_W; x++) {
        f = &feeders[x];
        if (f->y < 0) continue;

        if (f->throttle > 0) {
            f->throttle--;
            continue;
        }

        if ((unsigned char)f->y < GRID_H) {
            c = &cells[(unsigned char)f->y * GRID_W + x];
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
        if (f->y >= GRID_H) {
            f->y       = -1;
            f->remaining = 0;
        }
    }

    // --- Glow decay ---
    c = cells;
    for (idx = 0; idx < (unsigned int)GRID_W * GRID_H; idx++, c++) {
        if (c->glyph && c->glow > 0) {
            c->glow--;
            if (c->glow == GLOW_WHITE || c->glow == 0)
                c->dirty = 1;
        }
    }

    // --- Activate new feeders based on density setting ---
    // density 1=sparse: ~1 new/frame  2=normal: ~2  3=dense: ~4
    new_feeders = (density < 2) ? 1 : (density < 3) ? 2 : 4;
    while (new_feeders--) {
        x = (unsigned char)(rand() % GRID_W);
        f = &feeders[x];
        if (f->y >= 0) continue;
        f->y         = (signed char)(rand() % (GRID_H / 3));
        f->remaining = 5 + (unsigned char)(rand() % (GRID_H - 5));
        f->throttle  = (speed >= 3) ? 0 : (speed == 2) ? (unsigned char)(rand() % 4) : (unsigned char)(rand() % 8);
    }

    // --- Render dirty cells ---
    c = cells;
    for (y = 0; y < GRID_H; y++) {
        for (x = 0; x < GRID_W; x++, c++) {
            if (!c->dirty) continue;
            c->dirty = 0;
            if (!c->glyph) {
                erase_cell(x, y);
            } else {
                draw_cell(x, y, c->glyph - 1u,
                    c->glow > GLOW_WHITE ? 2 : c->glow > 0 ? 1 : 0);
            }
        }
    }

}

// -----------------------------------------------------------------------
// Config data and dialog
// -----------------------------------------------------------------------

// cfgdat[0..3] = "MATX" magic; [4] = density (1-3); [5] = speed (1-3); [6] = glyphset (1=binary, 2=hana)
// Must be exactly 64 bytes: the desktop manager reads/writes a fixed 64-byte config block.
_transfer char cfgdat[64]          = { 'M', 'A', 'T', 'X', 2, 2, 1 };
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
        while (!Msg_Send(cfg_prz, _sympid, _symmsg));
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

    // idle_skip: idles to wait between ticks (for slow speeds)
    // burst: anim_ticks to run per idle (for fast speeds)
    idle_skip = (speed == 1) ? 6 : 1;
    burst     = (speed == 3) ? 3 : 1;

    srand((unsigned int)Sys_Counter());

    build_font();

    // Reset grid
    memset(cells,   0, sizeof(cells));
    memset(feeders, 0, sizeof(feeders));
    for (i = 0; i < GRID_W; i++)
        feeders[i].y = -1;

    // Open fullscreen animation window
    empty_str[0] = 0;

    anim_ctrl[0].value  = 0;
    anim_ctrl[0].type   = C_AREA;
    anim_ctrl[0].bank   = -1;
    anim_ctrl[0].param  = AREA_16COLOR | COLOR_BLACK;
    anim_ctrl[0].x      = 0;
    anim_ctrl[0].y      = 0;
    anim_ctrl[0].w      = SCREEN_W;
    anim_ctrl[0].h      = SCREEN_H;
    anim_ctrl[0].unused = 0;

    memset(&anim_cg, 0, sizeof(anim_cg));
    anim_cg.controls = 1;
    anim_cg.pid      = _sympid;
    anim_cg.first    = &anim_ctrl[0];

    memset(&anim_win, 0, sizeof(anim_win));
    anim_win.state    = WIN_NORMAL;
    anim_win.flags    = WIN_NOTTASKBAR | WIN_NOTMOVEABLE;
    anim_win.pid      = _sympid;
    anim_win.w        = SCREEN_W;
    anim_win.h        = SCREEN_H;
    anim_win.wfull    = SCREEN_W;
    anim_win.hfull    = SCREEN_H;
    anim_win.wmin     = 32;
    anim_win.hmin     = 24;
    anim_win.wmax     = SCREEN_W;
    anim_win.hmax     = SCREEN_H;
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
                anim_tick(density, speed, glyph_base, nglyphs);
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
    char           init_tmp[64];

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
