#ifndef QUICK_UI_H_
#define QUICK_UI_H_
#include "term_types.h"
#include "event.h"
#include "ui_settings.h"
#define UI_ITEM_MAX 24
#define UI_VOICE_TEXT_MAX 16384
typedef enum { UI_CLOSED, UI_QUICK, UI_SETTINGS, UI_CONNECTION, UI_INPUT,
    UI_APPEARANCE, UI_TABS, UI_EDITOR, UI_BINDINGS, UI_VOICE } ui_page_t;
/* Shared with the voice coordinator; values are stable. */
typedef enum { UI_VOICE_IDLE=0, UI_VOICE_CONNECTING=1, UI_VOICE_RECORDING=2,
    UI_VOICE_FINALIZING=3, UI_VOICE_READY=4, UI_VOICE_ERROR=5,
    UI_VOICE_INSERTING=6 } ui_voice_state_t;
typedef enum { UI_ICON_NONE, UI_ICON_TERMINAL, UI_ICON_CLOUD, UI_ICON_KEYBOARD,
    UI_ICON_SETTINGS, UI_ICON_BACK } ui_icon_t;
typedef enum { UI_EFFECT_NONE, UI_EFFECT_MODE_NORMAL, UI_EFFECT_MODE_INSERT,
    UI_EFFECT_CLOUD, UI_EFFECT_PASTE, UI_EFFECT_INTERRUPT, UI_EFFECT_TAB_NEW,
    UI_EFFECT_TAB_SELECT, UI_EFFECT_SAVE, UI_EFFECT_BINDINGS, UI_EFFECT_EDITOR_PASTE,
    UI_EFFECT_VOICE_START, UI_EFFECT_VOICE_STOP, UI_EFFECT_VOICE_CANCEL,
    UI_EFFECT_VOICE_INSERT, UI_EFFECT_VOICE_TEST, UI_EFFECT_VOICE_PASTE,
    UI_EFFECT_VOICE_COPY } ui_effect_kind_t;
typedef struct {ui_effect_kind_t kind; session_id_t target; int arg;} ui_effect_t;
typedef struct {session_id_t id; term_input_mode_t mode; term_profile_t profile;} ui_tab_t;
typedef struct ui_item {
    rect_t rect;
    int action, value, selected;
    ui_icon_t icon;
    char label[160], detail[192];
} ui_item_t;
typedef struct quick_ui {
    ui_page_t page, editor_parent, connection_parent;
    ui_settings_t saved, draft;
    int w,h,scale, usable_h;
    rect_t ball;
    ui_item_t items[UI_ITEM_MAX]; int item_count;
    ui_tab_t tabs[8]; unsigned tab_count;
    session_id_t active, target;
    term_input_mode_t mode;
    term_profile_t profile;
    char title[96], status[192];
    int pointer_owned, ball_pointer, down_x, down_y, offset_x, offset_y, moved, down_item;
    int down_action, down_value;
    int dirty, save_pending;
    unsigned char blocked[8192];
    int alt,shift,sym, editor_field;
    char edit[257]; size_t cursor;
    char voice_text[UI_VOICE_TEXT_MAX+1];
    int voice_state, voice_edited, voice_scroll, voice_expanded;
    unsigned voice_revision;
    size_t voice_cursor;
    rect_t voice_rect, voice_capsule;
    ui_effect_t effect;
} quick_ui_t;

void quick_ui_init(quick_ui_t *u, const ui_settings_t *s);
void quick_ui_geometry(quick_ui_t *u,int w,int h,int keyboard_height);
void quick_ui_sync(quick_ui_t *u,const ui_tab_t *tabs,unsigned count,session_id_t active);
void quick_ui_open(quick_ui_t *u,ui_page_t page);
void quick_ui_close(quick_ui_t *u);
/* Terminal-only long-press entry: open a fresh draft and start immediately. */
int quick_ui_voice_shortcut(quick_ui_t *u);
/* One fully owned gesture; UI key presses remain blocked until release. */
int quick_ui_event(quick_ui_t *u,const event_t *event);
void quick_ui_layout(quick_ui_t *u);
int quick_ui_take_effect(quick_ui_t *u,ui_effect_t *out);
void quick_ui_save_result(quick_ui_t *u,int success);
void quick_ui_message(quick_ui_t *u,const char *text);
int quick_ui_active(const quick_ui_t *u);
/* Field paste has no PTY path; ASCII controls are rejected, input is bounded. */
int quick_ui_editor_paste(quick_ui_t *u,const char *text,size_t n);
/* Includes the connection/settings editor opened from a voice composition. */
int quick_ui_voice_context(const quick_ui_t *u);
/* Compact composition keeps the terminal visible; settings and the expanded
 * draft editor remain regular panels. All voice keys still belong to the UI. */
int quick_ui_voice_inline(const quick_ui_t *u);
/* Move the capsule and its hit regions together when avoiding the input line.
 * The preview rectangle is independently positioned by the terminal painter. */
void quick_ui_voice_capsule_position(quick_ui_t *u,int y);
/* Retain a draft for copying even when its original PTY no longer exists. */
void quick_ui_voice_target_lost(quick_ui_t *u);
/* Full snapshots, never suffixes. Final/error drafts and local edits reject
 * late recognizer snapshots. NULL text is a coordinator state-only update. */
void quick_ui_voice_update(quick_ui_t *u,int state,const char *text,unsigned revision,const char *status);
/* Insert valid UTF-8 at the composition cursor; has no terminal/PTY path. */
int quick_ui_voice_paste(quick_ui_t *u,const char *text,size_t n);
#endif
