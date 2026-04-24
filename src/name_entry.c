/**
 * name_entry.c - Name Entry Screen for Online Play
 *
 * Allows the player to enter a custom name using a character grid.
 * Supports A-Z, 0-9, '.', ':' (38 chars). Max name length: 16 chars.
 * D-pad navigates, A/C selects, B cancels, DEL removes, OK confirms.
 *
 * Adapted from Disasteroids name_entry.c for Utenyaa.
 * Uses custom sprite font for styled text rendering.
 */

#include <jo/jo.h>
#include "name_entry.h"
#include "connecting.h"
#include "font.h"
#include "net/utenyaa_game.h"
#include "net/utenyaa_net.h"

/*============================================================================
 * Character Grid Layout
 *
 * Row 0: A B C D E F G H I     (9 chars)
 * Row 1: J K L M N O P Q R     (9 chars)
 * Row 2: S T U V W X Y Z       (8 chars)
 * Row 3: 0 1 2 3 4 5 6 7 8 9   (10 chars)
 * Row 4: . : DEL OK             (4 items)
 *============================================================================*/

#define GRID_ROWS 5

static const char* g_grid_chars[] = {
    "ABCDEFGHI",   /* row 0: 9 */
    "JKLMNOPQR",   /* row 1: 9 */
    "STUVWXYZ",    /* row 2: 8 */
    "0123456789",  /* row 3: 10 */
    NULL           /* row 4: special (., :, DEL, OK) */
};

/* Row 4 has 4 items: '.', ':', DEL, OK */
#define ROW4_COUNT 4
#define ROW4_DEL   2
#define ROW4_OK    3

static int g_row_lengths[] = { 9, 9, 8, 10, ROW4_COUNT };

static char g_name_buf[UNET_MAX_NAME + 1];
static int g_name_len;
static int g_cursor_row;
static int g_cursor_col;
static int g_bkp_load_status;

/* Grid drawing origin in VDP1 coords */
#define GRID_X  FONT_X(6)
#define GRID_Y  FONT_Y(7)

/*============================================================================
 * Helpers
 *============================================================================*/

static int getRowLen(int row)
{
    if (row < 0 || row >= GRID_ROWS) return 0;
    return g_row_lengths[row];
}

static char getGridChar(int row, int col)
{
    if (row < 0 || row >= GRID_ROWS) return 0;
    if (col < 0 || col >= getRowLen(row)) return 0;

    if (row < 4) {
        return g_grid_chars[row][col];
    }

    if (col == 0) return '.';
    if (col == 1) return ':';
    return 0; /* DEL=2, OK=3 are actions */
}

/*============================================================================
 * Callbacks
 *============================================================================*/

void nameEntry_init(void)
{
    /* Clear VDP2 text plane */
    jo_clear_screen();

    /* Load saved name from backup RAM if no name set yet */
    g_bkp_load_status = 0;
    if (g_Game.playerName[0] == '\0') {
        if (jo_backup_mount(JoInternalMemoryBackup)) {
            if (jo_backup_file_exists(JoInternalMemoryBackup, "UTEN_NAME")) {
                unsigned int blen = 0;
                void* data = jo_backup_load_file_contents(
                    JoInternalMemoryBackup, "UTEN_NAME", &blen);
                if (data && blen > 0 && blen <= UNET_MAX_NAME + 1) {
                    memcpy(g_Game.playerName, data, blen);
                    g_Game.playerName[UNET_MAX_NAME] = '\0';
                    g_bkp_load_status = 1;
                } else {
                    g_bkp_load_status = 4;
                }
                if (data) jo_free(data);
            } else {
                g_bkp_load_status = 3;
            }
            jo_backup_unmount(JoInternalMemoryBackup);
        } else {
            g_bkp_load_status = 2;
        }
    }

    /* Pre-populate with previous name if available */
    if (g_Game.playerName[0] != '\0') {
        int i;
        g_name_len = 0;
        for (i = 0; g_Game.playerName[i] && i < UNET_MAX_NAME; i++) {
            g_name_buf[i] = g_Game.playerName[i];
            g_name_len++;
        }
        g_name_buf[g_name_len] = '\0';
    } else {
        g_name_buf[0] = '\0';
        g_name_len = 0;
    }
    g_cursor_row = 0;
    g_cursor_col = 0;

    /* Prevent carry-over from title screen button press */
    g_Game.input.pressedABC = true;
    g_Game.input.pressedStart = true;
    g_Game.input.pressedUp = true;
    g_Game.input.pressedDown = true;
    g_Game.input.pressedLeft = true;
    g_Game.input.pressedRight = true;
}

void nameEntry_input(void)
{
    int rowLen;

    if (g_Game.gameState != UGAME_STATE_NAME_ENTRY) return;

    /* D-pad: move cursor */
    if (jo_is_pad1_key_pressed(JO_KEY_UP)) {
        if (g_Game.input.pressedUp == false) {
            g_cursor_row--;
            if (g_cursor_row < 0) g_cursor_row = GRID_ROWS - 1;
            rowLen = getRowLen(g_cursor_row);
            if (g_cursor_col >= rowLen) g_cursor_col = rowLen - 1;
        }
        g_Game.input.pressedUp = true;
    } else {
        g_Game.input.pressedUp = false;
    }

    if (jo_is_pad1_key_pressed(JO_KEY_DOWN)) {
        if (g_Game.input.pressedDown == false) {
            g_cursor_row++;
            if (g_cursor_row >= GRID_ROWS) g_cursor_row = 0;
            rowLen = getRowLen(g_cursor_row);
            if (g_cursor_col >= rowLen) g_cursor_col = rowLen - 1;
        }
        g_Game.input.pressedDown = true;
    } else {
        g_Game.input.pressedDown = false;
    }

    if (jo_is_pad1_key_pressed(JO_KEY_LEFT)) {
        if (g_Game.input.pressedLeft == false) {
            g_cursor_col--;
            if (g_cursor_col < 0) g_cursor_col = getRowLen(g_cursor_row) - 1;
        }
        g_Game.input.pressedLeft = true;
    } else {
        g_Game.input.pressedLeft = false;
    }

    if (jo_is_pad1_key_pressed(JO_KEY_RIGHT)) {
        if (g_Game.input.pressedRight == false) {
            g_cursor_col++;
            if (g_cursor_col >= getRowLen(g_cursor_row)) g_cursor_col = 0;
        }
        g_Game.input.pressedRight = true;
    } else {
        g_Game.input.pressedRight = false;
    }

    /* A or C: select character / action */
    if (jo_is_pad1_key_pressed(JO_KEY_A) ||
        jo_is_pad1_key_pressed(JO_KEY_C)) {
        if (g_Game.input.pressedABC == false) {
            if (g_cursor_row == 4 && g_cursor_col == ROW4_DEL) {
                if (g_name_len > 0) {
                    g_name_len--;
                    g_name_buf[g_name_len] = '\0';
                }
            } else if (g_cursor_row == 4 && g_cursor_col == ROW4_OK) {
                if (g_name_len > 0) {
                    int i;
                    for (i = 0; i < g_name_len; i++)
                        g_Game.playerName[i] = g_name_buf[i];
                    g_Game.playerName[g_name_len] = '\0';

                    /* Save name to backup RAM */
                    {
                        static char bkp_fname[] = "UTEN_NAME";
                        static char bkp_comment[] = "Name";
                        if (jo_backup_mount(JoInternalMemoryBackup)) {
                            jo_backup_save_file_contents(
                                JoInternalMemoryBackup, bkp_fname,
                                bkp_comment, g_Game.playerName,
                                (unsigned int)(g_name_len + 1));
                            jo_backup_unmount(JoInternalMemoryBackup);
                        }
                    }

                    g_Game.isOnlineMode = true;
                    g_Game.gameState = UGAME_STATE_CONNECTING;
                    connecting_init();
                }
            } else {
                char ch = getGridChar(g_cursor_row, g_cursor_col);
                if (ch != 0 && g_name_len < UNET_MAX_NAME) {
                    g_name_buf[g_name_len] = ch;
                    g_name_len++;
                    g_name_buf[g_name_len] = '\0';
                }
            }
        }
        g_Game.input.pressedABC = true;
    } else {
        g_Game.input.pressedABC = false;
    }

    /* B: cancel back to title screen */
    if (jo_is_pad1_key_pressed(JO_KEY_B)) {
        if (g_Game.input.pressedLT == false) {
            jo_clear_screen();
            g_Game.input.pressedABC = true; /* block title screen from re-processing B */
            g_Game.titleScreenChoice = 2;   /* reset cursor away from ONLINE */
            g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
        }
        g_Game.input.pressedLT = true;
    } else {
        g_Game.input.pressedLT = false;
    }
}

void nameEntry_draw(void)
{
    int row, col;
    int gy;
    char ch_buf[2];

    if (g_Game.gameState != UGAME_STATE_NAME_ENTRY) return;

    /* Title */
    font_draw_centered("ENTER NAME", FONT_Y(3), 500);

    /* Character grid - draw each character individually for cursor highlight */
    for (row = 0; row < GRID_ROWS; row++) {
        int rowLen = getRowLen(row);
        gy = GRID_Y + row * 16;  /* 2 rows spacing between grid rows */

        for (col = 0; col < rowLen; col++) {
            int gx;
            const char* label = NULL;

            if (row < 4) {
                /* Regular character: spaced at 16px intervals */
                gx = GRID_X + col * 16;
                ch_buf[0] = g_grid_chars[row][col];
                ch_buf[1] = '\0';
                label = ch_buf;
            } else {
                /* Row 4: special items with variable spacing */
                static const int row4_px[] = {0, 24, 48, 88};
                gx = GRID_X + row4_px[col];
                switch (col) {
                    case 0: label = "."; break;
                    case 1: label = ":"; break;
                    case 2: label = "DEL"; break;
                    case 3: label = "OK"; break;
                }
            }

            if (label) {
                font_draw(label, gx, gy, 500);
            }
        }

        /* Draw cursor '>' indicator for selected row */
        if (row == g_cursor_row) {
            font_draw(">", GRID_X - 16, gy, 501);
        }

        /* Draw '^' caret under selected column */
        if (row == g_cursor_row) {
            int cx;
            if (row < 4) {
                cx = GRID_X + g_cursor_col * 16;
            } else {
                static const int row4_px[] = {0, 24, 48, 88};
                cx = GRID_X + row4_px[g_cursor_col];
            }
            font_draw("^", cx, gy + 8, 501);
        }
    }

    /* Show selected character preview on the right */
    {
        int preview_x = FONT_X(28);
        int preview_y = GRID_Y + g_cursor_row * 16;

        if (g_cursor_row < 4) {
            char sel = getGridChar(g_cursor_row, g_cursor_col);
            if (sel) {
                char prev[4];
                prev[0] = '[';
                prev[1] = sel;
                prev[2] = ']';
                prev[3] = '\0';
                font_draw(prev, preview_x, preview_y, 500);
            }
        } else {
            static const char* labels[] = { "[.]", "[:]", "[DEL]", "[OK]" };
            font_draw(labels[g_cursor_col], preview_x, preview_y, 500);
        }
    }

    /* Display current name */
    font_printf(FONT_X(6), FONT_Y(19), 500, "NAME: %s", g_name_buf);

    /* Name length indicator */
    font_printf(FONT_X(6), FONT_Y(20), 500, "      %d/%d", g_name_len, UNET_MAX_NAME);

    /* Controls hint */
    if (g_name_len > 0) {
        font_draw("A/C:SELECT B:BACK OK:CONFIRM", FONT_X(1), FONT_Y(26), 500);
    } else {
        font_draw("A/C:SELECT  B:CANCEL", FONT_X(1), FONT_Y(26), 500);
    }

    /* Backup RAM status */
    if (g_bkp_load_status > 0) {
        static const char* bkp_msgs[] = {
            "", "SAVE LOADED OK", "BKP MOUNT FAIL",
            "NO SAVE FOUND", "BKP READ FAIL"
        };
        font_draw(bkp_msgs[g_bkp_load_status], FONT_X(1), FONT_Y(27), 500);
    }
}
