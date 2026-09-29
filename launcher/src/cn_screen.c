#include "cn_screen.h"
#include "cjk_font.h"

#include <ctype.h>
#include <debug.h>
#include <ee_regs.h>
#include <kernel.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* PS2SDK scr_printf.c configures a 640x224 debug surface. */
#define CN_SCREEN_WIDTH       640
#define CN_SCREEN_HEIGHT      224
#define CN_ASCII_WIDTH        8
#define CN_CJK_WIDTH          16
#define CN_CELL_HEIGHT        16
#define CN_TOP_MARGIN         32
#define CN_TEXT_BUFFER_SIZE   2048

static int cn_x;
static int cn_y;
static int cn_initialized;
static uint32_t cn_bgcolor = 0x000000;
static uint32_t cn_fontcolor = 0xFFFFFF;

/* Same default palette used by PS2SDK scr_printf.c. */
static const uint32_t cn_defcols[16] = {
    0x000000, 0xDA3700, 0xDD963A, 0x981788,
    0x1F0FC5, 0xCCCCCC, 0x009CC1, 0x767676,
    0xFF783B, 0xD6D661, 0x0CC616, 0x9E00B4,
    0x5648E7, 0xF2F2F2, 0xA5F1F9, 0xFFFFFF,
};

typedef struct cn_setupchar {
    uint64_t dd0[4];
    uint32_t dw0[1];
    uint16_t x, y;
    uint64_t dd1[1];
    uint32_t dw1[2];
    uint64_t dd2[5];
} cn_setupchar;

/* Same packet structure as PS2SDK scr_putchar(), but the image transfer is
 * 16x16 pixels instead of 8x8, so the IMAGE GIFtag contains 64 qwords. */
static const cn_setupchar cn_setupchar_template = {
    {0x1000000000000004ULL, 0xEULL, 0xA000000000000ULL, 0x50ULL},
    {0},
    100,
    100,
    {0x51ULL},
    {16, 16},
    {0x52ULL, 0, 0x53ULL, 0x800000000008040ULL, 0},
};

static inline void cn_dma_wait(void) {
    while ((*R_EE_D2_CHCR & 0x100) != 0)
        ;
}

static inline void cn_progdma(const void *addr, int qwc) {
    *R_EE_D2_QWC = (uint32_t)qwc;
    *R_EE_D2_MADR = (uint32_t)(uintptr_t)addr;
    *R_EE_D2_CHCR = 0x101;
}

static int cn_hex2int(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';

    c &= (char)~0x20;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

static void cn_clear_line(int y) {
    int x;

    /* Clear exactly one 16-pixel row using the original 8x8 PS2SDK glyph. */
    for (x = 0; x < CN_SCREEN_WIDTH; x += CN_ASCII_WIDTH) {
        scr_putchar(x, y, cn_bgcolor, ' ');
        scr_putchar(x, y + 8, cn_bgcolor, ' ');
    }
}

static void cn_advance_line(void) {
    cn_x = 0;
    cn_y += CN_CELL_HEIGHT;
    if (cn_y >= CN_SCREEN_HEIGHT)
        cn_y = CN_TOP_MARGIN;

    cn_clear_line(cn_y);
}

static void cn_draw_cjk(int x, int y, uint32_t color, const uint8_t *glyph) {
    static cn_setupchar setupchar __attribute__((aligned(16)));
    static uint32_t charmap[16 * 16] __attribute__((aligned(16)));
    int row, col;

    setupchar = cn_setupchar_template;
    setupchar.x = (uint16_t)x;
    setupchar.y = (uint16_t)y;

    *((cn_setupchar *)UNCACHED_SEG(&setupchar)) = setupchar;
    cn_progdma(&setupchar, 6);

    for (row = 0; row < 16; row++) {
        for (col = 0; col < 16; col++) {
            const uint8_t bits = glyph[row * 2 + (col >> 3)];
            const uint32_t pixel = (bits & (0x80 >> (col & 7)))
                                       ? color
                                       : cn_bgcolor;
            *(uint32_t *)UNCACHED_SEG(&charmap[row * 16 + col]) = pixel;
        }
    }

    cn_dma_wait();
    cn_progdma(charmap, 64); /* 16x16 pixels * 4 bytes / 16 = 64 qwords */
    cn_dma_wait();
}

/* Decode exactly one UTF-8 sequence within [src, end). Invalid or truncated
 * sequences consume one byte and return U+FFFD. */
static uint32_t cn_utf8_next(const unsigned char **src,
                             const unsigned char *end) {
    const unsigned char *s = *src;
    uint32_t cp;

    if (s >= end)
        return 0;

    if (s[0] < 0x80) {
        cp = s[0];
        *src = s + 1;
        return cp;
    }

    if (s + 2 <= end &&
        (s[0] & 0xE0) == 0xC0 &&
        (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) |
             (uint32_t)(s[1] & 0x3F);
        if (cp >= 0x80) {
            *src = s + 2;
            return cp;
        }
    }

    if (s + 3 <= end &&
        (s[0] & 0xF0) == 0xE0 &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) |
             (uint32_t)(s[2] & 0x3F);
        if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
            *src = s + 3;
            return cp;
        }
    }

    if (s + 4 <= end &&
        (s[0] & 0xF8) == 0xF0 &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) |
             ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) |
             (uint32_t)(s[3] & 0x3F);
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            *src = s + 4;
            return cp;
        }
    }

    *src = s + 1;
    return 0xFFFD;
}

static void cn_draw_ascii(uint32_t cp) {
    if (cn_x + CN_ASCII_WIDTH > CN_SCREEN_WIDTH)
        cn_advance_line();

    /* Center PS2SDK's 8x8 glyph inside the 8x16 text cell. */
    scr_putchar(cn_x, cn_y + 4, cn_fontcolor, (int)cp);
    cn_x += CN_ASCII_WIDTH;
}

static void cn_draw_codepoint(uint32_t cp) {
    const uint8_t *glyph;

    if (cp < 0x80) {
        cn_draw_ascii(cp);
        return;
    }

    glyph = cjk_font_find(cp);
    if (glyph != NULL) {
        if (cn_x + CN_CJK_WIDTH > CN_SCREEN_WIDTH)
            cn_advance_line();

        cn_draw_cjk(cn_x, cn_y, cn_fontcolor, glyph);
        cn_x += CN_CJK_WIDTH;
        return;
    }

    /* Unknown characters remain visible instead of silently disappearing. */
    cn_draw_ascii('?');
}

void cn_screen_init(void) {
    if (cn_initialized)
        return;

    /* Initialise the same GS/debug surface as PS2SDK's scr_printf(). */
    init_scr();
    scr_setCursor(0);
    scr_setbgcolor(cn_bgcolor);
    scr_setfontcolor(cn_fontcolor);

    /* Preserve launcHER's original overscan guard. */
    scr_putchar(0, 0, cn_fontcolor, '.');
    cn_x = 0;
    cn_y = CN_TOP_MARGIN;
    cn_initialized = 1;
}

void cn_screen_vprintf(const char *format, va_list args) {
    char buff[CN_TEXT_BUFFER_SIZE];
    int len;
    const unsigned char *p;
    const unsigned char *end;

    cn_screen_init();

    len = vsnprintf(buff, sizeof(buff), format, args);
    if (len < 0)
        return;
    if ((size_t)len >= sizeof(buff))
        len = (int)sizeof(buff) - 1;

    p = (const unsigned char *)buff;
    end = p + len;

    while (p < end) {
        const unsigned char *before = p;
        uint32_t cp;

        if (*p == '\n') {
            cn_advance_line();
            p++;
            continue;
        }

        if (*p == '\r') {
            cn_x = 0;
            p++;
            continue;
        }

        if (*p == '\t') {
            int j;
            for (j = 0; j < 5; j++)
                cn_draw_ascii(' ');
            p++;
            continue;
        }

        /* Preserve PS2SDK's two-hex-digit colour escape convention. */
        if (*p == 0x1b &&
            p + 3 <= end &&
            isxdigit((unsigned char)p[1]) &&
            isxdigit((unsigned char)p[2])) {
            const int bg = cn_hex2int((char)p[1]);
            const int fg = cn_hex2int((char)p[2]);
            if (bg >= 0)
                cn_bgcolor = cn_defcols[bg];
            if (fg >= 0)
                cn_fontcolor = cn_defcols[fg];
            scr_setbgcolor(cn_bgcolor);
            scr_setfontcolor(cn_fontcolor);
            p += 3;
            continue;
        }

        cp = cn_utf8_next(&p, end);
        cn_draw_codepoint(cp);

        if (p == before)
            p++;
    }
}
