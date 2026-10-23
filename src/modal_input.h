#ifndef MODAL_INPUT_H_
#define MODAL_INPUT_H_

#include "app.h"

/* Transient physical-key state, separate from each session's persistent mode.
 * Keys consumed in NORMAL remain blocked through release, even across tabs,
 * reloads or mode changes. No pointers into reloadable preferences. */
typedef struct modal_input {
	unsigned char blocked[8192]; /* BB10 keysyms are 16-bit; no slot overflow */
	int hold_key;
	session_id_t hold_session;
	unsigned prefix;
	unsigned consumed_mods; /* output: inherited one-shot modifiers to clear */
} modal_input_t;

/* Render-loop snapshot: a tab/mode change requests a full repaint even if
 * the mode setter was called outside keyboard dispatch. */
typedef struct modal_view {
	session_id_t session;
	term_input_mode_t mode;
	term_profile_t profile;
} modal_view_t;
int modal_view_update(modal_view_t *view, app_t *app);

typedef int (*modal_dispatch_fn)(app_t *, const action_t *);
void modal_input_block(modal_input_t *state, int sym);
int modal_is_mode_action(const action_t *a);
/* 1 = consume completely, 0 = continue the unchanged INSERT pipeline.
 * Call for releases too, before help/metamode/keyhold/terminal fallback.
 * legacy_mods is the shared latched modifier snapshot (including Q20's Ctrl
 * chord). After a consumed event, clear state->consumed_mods in that layer. */
int modal_input_handle(modal_input_t *state, app_t *app, const pref_t *prefs,
                       const key_event_t *key, unsigned legacy_mods,
                       modal_dispatch_fn dispatch);
/* Shared mode-action implementation; UI caller repaints on success. */
int modal_apply_action(app_t *app, const action_t *a);

#endif
