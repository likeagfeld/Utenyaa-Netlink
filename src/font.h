#pragma once
/*
 * font.h - Custom sprite font for online screens
 *
 * Styled text rendering via VDP1 sprites (8x8 tiles loaded from FONT.TGA).
 * Used by connecting, name_entry, and lobby screens. Covers ASCII 32-95
 * (space, punctuation, digits, uppercase A-Z, underscore). Lowercase is
 * auto-uppercased at draw time.
 */

#include <jo/jo.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_CHAR_W  8
#define FONT_CHAR_H  8
#define FONT_FIRST   32
#define FONT_LAST    95
#define FONT_COUNT   64

/* Convert jo_printf grid coords to VDP1 pixel coords (top-left) */
#define FONT_X(col)  (-160 + (col) * 8)
#define FONT_Y(row)  (-112 + (row) * 8)

void font_load(void);
void font_draw(const char* str, int x, int y, int z);
void font_draw_centered(const char* str, int y, int z);
void font_printf(int x, int y, int z, const char* fmt, ...);
void font_printf_centered(int y, int z, const char* fmt, ...);

#ifdef __cplusplus
}
#endif
