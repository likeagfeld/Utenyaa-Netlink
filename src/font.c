/*
 * font.c - Online-screen text rendering via jo_printf (VDP2 NBG text)
 *
 * The sprite-font path that ships with Disasteroids / Flicky's Flock
 * (jo_sprite_add_tga_tileset of FONT.TGA) can't be used on Utenyaa
 * without blowing JO_MAX_SPRITE — Utenyaa already uses ~103 of the
 * 100 upstream sprite slots just for PAKs + models, and bumping the
 * cap triggers pool OOM on boot. Instead, we render online-screen
 * text through jo_printf which writes to a VDP2 NBG character plane
 * and consumes zero sprite slots.
 *
 * The API shape (font_draw / font_draw_centered / font_printf /
 * font_printf_centered with VDP1 pixel coords) is preserved so the
 * connecting / name_entry / lobby screens don't need to change —
 * the pixel coords are converted to the jo_printf 40×28 character
 * grid at draw time.
 */

#include "font.h"
#include <stdarg.h>
#include <stdio.h>

/* Raise NBG0 text-plane priority so jo_printf output is visible above
 * the backdrop. Utenyaa's main.cxx calls jo_core_set_screens_order with
 * only NBG1 + SPRITE, which leaves NBG0 at priority 0 (hidden).
 *
 * We use priority 4 — above the backdrop, but BELOW the default sprite
 * priority of ~7. That layering lets jo_sprite_draw3D-rendered VDP1
 * quads (e.g., the per-player character icons in the lobby) draw ON
 * TOP of the NBG0 text plane. Without this, NBG0 at priority 7 would
 * occlude sprites entirely on every cell (even cells with no glyph,
 * since NBG0 paints opaque backdrop on empty cells by default). */
void font_load(void)
{
    slPriorityNbg0(4);
}

/* jo_printf uses a 40-col × 28-row text grid. Our API takes VDP1
 * pixel coords. Convert: +160/+112 to shift from VDP1-centred to
 * top-left, then divide by 8 for the cell grid. */
static inline int px_to_col(int x) { return (x + 160) / 8; }
static inline int px_to_row(int y) { return (y + 112) / 8; }

static void draw_line(int col, int row, const char* s)
{
    if (!s) return;
    if (row < 0 || row >= 28) return;
    char buf[41];
    int i = 0, x = col;
    while (s[i] && x < 40) {
        buf[i] = s[i];
        i++; x++;
    }
    buf[i] = '\0';
    if (col >= 0 && col < 40)
        jo_printf(col, row, "%s", buf);
}

void font_draw(const char* str, int x, int y, int /*z*/)
{
    draw_line(px_to_col(x), px_to_row(y), str);
}

void font_draw_centered(const char* str, int y, int /*z*/)
{
    if (!str) return;
    int len = 0;
    while (str[len]) len++;
    int col = 20 - len / 2;
    if (col < 0) col = 0;
    draw_line(col, px_to_row(y), str);
}

void font_printf(int x, int y, int /*z*/, const char* fmt, ...)
{
    static char buf[80];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    draw_line(px_to_col(x), px_to_row(y), buf);
}

void font_printf_centered(int y, int /*z*/, const char* fmt, ...)
{
    static char buf[80];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    int len = 0;
    while (buf[len]) len++;
    int col = 20 - len / 2;
    if (col < 0) col = 0;
    draw_line(col, px_to_row(y), buf);
}
