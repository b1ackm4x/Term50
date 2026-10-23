#ifndef UI_SETTINGS_H_
#define UI_SETTINGS_H_
#include <stddef.h>

/* Private, data-only settings. No executable Lua and no terminal keymap edits. */
typedef struct ui_settings {
    int side;              /* 0 left, 1 right */
    int height_permille;   /* normalized available travel, not raw pixels */
    int opacity;           /* 30..85, while resting */
    int edge_handle;
    int default_profile;      /* -1 inherit Lua, otherwise term_profile_t */
    int companion_enabled;
    int companion_port;
    char companion_host[64];
    char companion_token[257]; /* empty while disabled, or 64 hex; never rendered or logged */
} ui_settings_t;

void ui_settings_defaults(ui_settings_t *out);
int ui_settings_valid(const ui_settings_t *s);
/* Missing file is success with defaults; corrupt file is failure, defaults kept. */
int ui_settings_load(const char *directory, ui_settings_t *out);
/* Atomic replacement in a private directory; no partially accepted settings. */
int ui_settings_save(const char *directory, const ui_settings_t *s);
#endif
