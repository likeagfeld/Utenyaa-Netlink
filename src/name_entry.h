#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void nameEntry_init(void);
/* Like nameEntry_init but skips the saved-name backup-RAM load and
 * starts the entry buffer empty. Used after the Download-Characters
 * flow so the placeholder "DL" username (which may have been
 * inadvertently saved to backup in an earlier session if the user
 * confirmed the prefilled value) doesn't leak into the next
 * online session's name. */
void nameEntry_init_blank(void);
void nameEntry_input(void);
void nameEntry_draw(void);

#ifdef __cplusplus
}
#endif
