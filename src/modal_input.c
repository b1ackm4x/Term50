#include <sys/keycodes.h>
#include <string.h>
#include "modal_input.h"
#include "terminal.h"

int modal_view_update(modal_view_t *view, app_t *app) {
	session_t *s = app_active_session(app);
	session_id_t id = session_id(s);
	term_input_mode_t mode = session_input_mode(s);
	if (view->session == id && view->mode == mode && view->profile == session_profile(s)) return 0;
	view->session = id;
	view->mode = mode;
	view->profile = session_profile(s);
	return 1;
}

int modal_is_mode_action(const action_t *a) {
	return action_is_builtin(a, TERM_BUILTIN_MODE_NORMAL) ||
	       action_is_builtin(a, TERM_BUILTIN_MODE_INSERT) ||
	       action_is_builtin(a, TERM_BUILTIN_MODE_TOGGLE) ||
	       action_is_builtin(a, TERM_BUILTIN_PROFILE_TERMINAL) ||
	       action_is_builtin(a, TERM_BUILTIN_PROFILE_CLAUDE) ||
	       action_is_builtin(a, TERM_BUILTIN_PROFILE_TTY);
}

int modal_apply_action(app_t *app, const action_t *a) {
	session_t *s = app_active_session(app);
	if (!s || !modal_is_mode_action(a)) return 0;
	if (a->as.builtin.id == TERM_BUILTIN_PROFILE_TERMINAL) {session_set_profile(s,TERM_PROFILE_TERMINAL);return 1;}
	if (a->as.builtin.id == TERM_BUILTIN_PROFILE_CLAUDE) {session_set_profile(s,TERM_PROFILE_CLAUDE);return 1;}
	if (a->as.builtin.id == TERM_BUILTIN_PROFILE_TTY) {session_set_profile(s,TERM_PROFILE_TTY);return 1;}
	term_input_mode_t mode = TERM_INPUT_INSERT;
	if (a->as.builtin.id == TERM_BUILTIN_MODE_NORMAL) mode = TERM_INPUT_NORMAL;
	if (a->as.builtin.id == TERM_BUILTIN_MODE_TOGGLE)
		mode = session_input_mode(s) == TERM_INPUT_INSERT ? TERM_INPUT_NORMAL : TERM_INPUT_INSERT;
	session_set_input_mode(s, mode);
	return 1;
}

static unsigned modifier(int sym) {
	switch (sym) {
	case KEYCODE_LEFT_SHIFT: case KEYCODE_RIGHT_SHIFT: return KEYMOD_SHIFT;
	case KEYCODE_LEFT_ALT: case KEYCODE_RIGHT_ALT: return KEYMOD_ALT;
	case KEYCODE_LEFT_CTRL: case KEYCODE_RIGHT_CTRL: return KEYMOD_CTRL;
	case KEYCODE_BB_SYM_KEY: return CHORD_MOD_SYM;
	default: return 0;
	}
}

void modal_input_block(modal_input_t *st, int sym) {
	if (sym > 0 && sym <= 0xffff)
		st->blocked[(unsigned)sym >> 3] |= (unsigned char)(1u << (sym & 7));
}

/* NORMAL still operates the foreground program. Keep text behind bindings,
 * but send terminal editing/navigation keys and ASCII Ctrl combinations
 * directly, without entering the INSERT metamode/keyhold pipeline. These VT
 * sequences match the physical-key encoder used by INSERT. */
static int normal_terminal_input(session_t *s, int sym, unsigned mods) {
	const char *bytes = NULL;
	unsigned char control;
	switch (sym) {
	case KEYCODE_RETURN: case KEYCODE_KP_ENTER: bytes = "\r"; break;
	case KEYCODE_ESCAPE: bytes = "\x1b"; break;
	case KEYCODE_TAB: bytes = mods & KEYMOD_SHIFT ? "\x1b[Z" : "\t"; break;
	case KEYCODE_BACK_TAB: bytes = "\x1b[Z"; break;
	case KEYCODE_BACKSPACE: bytes = "\b"; break;
	case KEYCODE_DELETE: bytes = "\x1b[3~"; break;
	case KEYCODE_INSERT: bytes = "\x1b[2~"; break;
	case KEYCODE_UP: bytes = "\x1b[A"; break;
	case KEYCODE_DOWN: bytes = "\x1b[B"; break;
	case KEYCODE_LEFT: bytes = "\x1b[D"; break;
	case KEYCODE_RIGHT: bytes = "\x1b[C"; break;
	case KEYCODE_HOME: bytes = "\x1b[H"; break;
	case KEYCODE_END: bytes = "\x1b[F"; break;
	case KEYCODE_PG_UP: bytes = "\x1b[5~"; break;
	case KEYCODE_PG_DOWN: bytes = "\x1b[6~"; break;
	}
	if (bytes) {
		session_write_bytes(s, bytes, strlen(bytes));
		return 1;
	}
	/* Some event sources already supply the C0/DEL symbol itself. */
	if ((sym > 0 && sym < 0x20) || sym == 0x7f) {
		control = (unsigned char)sym;
	} else if ((mods & KEYMOD_CTRL) && !(mods & (KEYMOD_ALT | CHORD_MOD_SYM))) {
		if (sym >= 'a' && sym <= 'z') control = (unsigned char)(sym - 'a' + 1);
		else if (sym >= '@' && sym <= '_') control = (unsigned char)(sym & 0x1f);
		else if (sym == ' ') control = 0;
		else if (sym == '?') control = 0x7f;
		else if (sym == '`' && (mods & KEYMOD_SHIFT)) control = 0x1e;
		else if (sym == '/' && (mods & KEYMOD_SHIFT)) control = 0x1f;
		else return 0;
	} else return 0;
	session_write_bytes(s, (const char *)&control, 1);
	return 1;
}

int modal_input_handle(modal_input_t *st, app_t *app, const pref_t *p,
                       const key_event_t *k, unsigned legacy_mods,
                       modal_dispatch_fn dispatch) {
	session_t *s = app_active_session(app);
	st->consumed_mods = 0;
	int sym = k->sym;
	int blocked = 0;
	if (sym > 0 && sym <= 0xffff) {
		unsigned char *slot = &st->blocked[(unsigned)sym >> 3];
		unsigned char bit = (unsigned char)(1u << (sym & 7));
		blocked = (*slot & bit) != 0;
		/* Virtual keyboards can omit key-up; a fresh down is a new gesture. */
		if (!k->pressed || !k->repeat) *slot &= (unsigned char)~bit;
	}
	if (!k->pressed) {
		if (st->hold_key == sym) st->hold_key = 0;
		return 1;
	}
	if (!k->repeat && st->hold_key == sym) st->hold_key = 0;
	if (!s) return 1;
	int normal = session_input_mode(s) == TERM_INPUT_NORMAL;
	if (k->repeat && blocked) {
		if (normal && st->hold_key == sym && st->hold_session == session_id(s)
		    && p->normal_insert_hold_enabled && p->normal_insert_hold_key == sym) {
			action_t a;
			action_parse("mode_insert", &a);
			st->hold_key = 0;
			st->prefix = 0;
			dispatch(app, &a);
		}
		return 1;
	}
	unsigned self = modifier(sym);
	unsigned mods = ((unsigned)k->modifiers & (KEYMOD_SHIFT | KEYMOD_CTRL | KEYMOD_ALT))
	              | legacy_mods | (normal ? st->prefix : 0);
	int trigger = sym == KEYCODE_RIGHT_SHIFT ? KEYCODE_LEFT_SHIFT : sym;
	chord_t *chord = self ? chord_lookup_subset(trigger, mods & ~self, p->chord_bindings)
	                     : chord_lookup(trigger, mods & (KEYMOD_SHIFT | KEYMOD_CTRL | KEYMOD_ALT | CHORD_MOD_SYM), p->chord_bindings);
	/* INSERT intercepts only explicitly configured mode chords. Everything
	 * else, including Esc and the old Shift+Sym default, keeps its old path. */
	if (!k->repeat && chord && (normal || modal_is_mode_action(&chord->action))) {
		modal_input_block(st, sym);
		st->prefix = 0;
		st->hold_key = 0;
		st->consumed_mods = legacy_mods;
		dispatch(app, &chord->action);
		return 1;
	}
	if (!normal) return 0;
	/* NORMAL never reaches any legacy layer, even on a failed action. */
	if (k->repeat) return 1;
	modal_input_block(st, sym);
	if (p->normal_insert_hold_enabled && sym == p->normal_insert_hold_key && (mods & ~self) == 0) {
		st->prefix = 0;
		st->hold_key = sym;
		st->hold_session = session_id(s);
		return 1;
	}
	if (self) { st->prefix ^= self; return 1; }
	st->prefix = 0;
	st->consumed_mods = legacy_mods;
	if (normal_terminal_input(s, sym, mods)) return 1;
	int glyph = k->unicode > 0 ? k->unicode : sym;
	if ((mods & KEYMOD_ALT) && sym > 0 && sym < 128) {
		keymap_t *alt = keymap_lookup((char)sym, p->altsym_entries);
		if (!alt || !alt->to[0] || alt->to[1]) return 1;
		glyph = (unsigned char)alt->to[0];
	} else if (mods & (KEYMOD_CTRL | CHORD_MOD_SYM)) {
		return 1;
	} else if ((mods & KEYMOD_SHIFT) && glyph >= 'a' && glyph <= 'z') {
		glyph -= 'a' - 'A';
	}
	/* Never truncate special/Unicode keycodes into a matching ASCII key. */
	if (glyph <= 0 || glyph >= 128) return 1;
	keymap_t *km = keymap_lookup((char)glyph,
	    session_profile(s) == TERM_PROFILE_TERMINAL ? p->terminal_keys : p->normal_keys);
	if (km) {
		session_id_t origin = session_id(s);
		int next = km->next_mode; /* dispatch may switch/close the originating tab */
		int ok;
		if (km->action.kind == TERM_ACTION_SEND_BYTES) {
			ok = session_write_bytes(s, km->action.as.bytes.data, km->action.as.bytes.len)
			     == (ssize_t)km->action.as.bytes.len;
		} else {
			ok = dispatch(app, &km->action);
		}
		if (ok && next) {
			s = app_session_by_id(app, origin);
			if (s) session_set_input_mode(s, (term_input_mode_t)(next - 1));
		}
	}
	return 1;
}
