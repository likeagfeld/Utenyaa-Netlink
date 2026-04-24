/*
 * font.c - Custom sprite font for online screens
 *
 * Loads FONT.TGA as a tileset and provides string drawing functions
 * using VDP1 sprites instead of VDP2 jo_printf text. Ported verbatim
 * from the Flicky's Flock / Disasteroids implementation.
 */

#include "font.h"
#include <stdarg.h>
#include <stdio.h>

static int g_fontBase = -1;

void font_load(void)
{
    jo_tile tiles[FONT_COUNT];
    int i;

    for (i = 0; i < FONT_COUNT; i++) {
        tiles[i].x = i * FONT_CHAR_W;
        tiles[i].y = 0;
        tiles[i].width = FONT_CHAR_W;
        tiles[i].height = FONT_CHAR_H;
    }

    g_fontBase = jo_sprite_add_tga_tileset(JO_ROOT_DIR, "FONT.TGA",
                                           JO_COLOR_Transparent,
                                           tiles, FONT_COUNT);
}

void font_draw(const char* str, int x, int y, int z)
{
    unsigned char c;

    if (g_fontBase < 0 || !str) return;

    while (*str) {
        c = (unsigned char)*str;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c >= FONT_FIRST && c <= FONT_LAST) {
            jo_sprite_draw3D(g_fontBase + (c - FONT_FIRST),
                             x + (FONT_CHAR_W / 2),
                             y + (FONT_CHAR_H / 2), z);
        }
        x += FONT_CHAR_W;
        str++;
    }
}

void font_draw_centered(const char* str, int y, int z)
{
    int len = 0;
    const char* p = str;
    if (!str) return;
    while (*p++) len++;
    font_draw(str, -(len * FONT_CHAR_W) / 2, y, z);
}

void font_printf(int x, int y, int z, const char* fmt, ...)
{
    static char buf[80];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    font_draw(buf, x, y, z);
}

void font_printf_centered(int y, int z, const char* fmt, ...)
{
    static char buf[80];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    font_draw_centered(buf, y, z);
}
