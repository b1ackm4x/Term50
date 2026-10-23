/*
 * Each session owns its terminal bridge, PTY, child and input mode.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <clipboard/clipboard.h>

#include "session.h"
#include <errno.h>
#include "ghostty_bridge.h"
#include "io.h"
#include "terminal.h"

enum { REPLY_CAPACITY = 16384, RECOVERY_CAPACITY = 16 };

struct session {
	session_id_t id;
	term_input_mode_t input_mode; /* calloc default: INSERT */
	term_profile_t normal_profile;
	ghostty_bridge_t *bridge;
	int master_fd;          /* -1 until session_set_master_fd() runs */
	pid_t child_pid;        /* 0 until session_set_child_pid() runs */
	int exited;             /* set by session_mark_exited via SIGCHLD reaper */
	int exit_status;        /* raw waitpid() status; valid when exited */
	uint64_t bytes_in;      /* total bytes read from the pty (shell -> term) */
	uint64_t bytes_out;     /* total bytes written to the pty (term -> shell) */
	uint64_t input_generation;
	int write_exclusive;
	char replies[REPLY_CAPACITY + RECOVERY_CAPACITY];
	size_t reply_size, reply_offset;
	size_t reply_recovery;  /* unwritten prefix, excluded from reply capacity */
	int reply_error;        /* consumed only by session_flush_replies */
};

int session_create(session_t **out, session_id_t id,
                   uint16_t cols, uint16_t rows, size_t max_scrollback) {
	session_t *s;
	if (out == NULL) {
		return -1;
	}
	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return -1;
	}
	s->id = id;
	s->normal_profile = TERM_PROFILE_CLAUDE; /* preserve existing NORMAL bindings */
	s->master_fd = -1;
	if (ghostty_bridge_create(&s->bridge, cols, rows, max_scrollback, s) != 0) {
		free(s);
		return -1;
	}
	*out = s;
	return 0;
}

void session_destroy(session_t *s) {
	if (s == NULL) {
		return;
	}
	if (s->master_fd >= 0) {
		close(s->master_fd);
		s->master_fd = -1;
	}
	ghostty_bridge_destroy(s->bridge);
	free(s);
}

term_input_mode_t session_input_mode(const session_t *s) {
	return s ? s->input_mode : TERM_INPUT_INSERT;
}

term_profile_t session_profile(const session_t *s) {
	return s && s->input_mode == TERM_INPUT_NORMAL ? s->normal_profile : TERM_PROFILE_TTY;
}

int session_set_profile(session_t *s, term_profile_t profile) {
	if (!s || profile < TERM_PROFILE_TERMINAL || profile > TERM_PROFILE_TTY) return 0;
	term_profile_t old = session_profile(s);
	if (profile != TERM_PROFILE_TTY) s->normal_profile = profile;
	s->input_mode = profile == TERM_PROFILE_TTY ? TERM_INPUT_INSERT : TERM_INPUT_NORMAL;
	return old != profile;
}

int session_set_input_mode(session_t *s, term_input_mode_t mode) {
	if (!s || (mode != TERM_INPUT_INSERT && mode != TERM_INPUT_NORMAL) || s->input_mode == mode)
		return 0;
	s->input_mode = mode;
	return 1;
}

session_id_t session_id(const session_t *s) {
	return s ? s->id : 0;
}

ghostty_bridge_t *session_bridge(session_t *s) {
	return s ? s->bridge : NULL;
}

int session_master_fd(const session_t *s) {
	return s ? s->master_fd : -1;
}

void session_set_master_fd(session_t *s, int fd) {
	if (s == NULL) {
		return;
	}
	s->master_fd = fd;
}

pid_t session_child_pid(const session_t *s) {
	return s ? s->child_pid : 0;
}

void session_set_child_pid(session_t *s, pid_t pid) {
	if (s == NULL) {
		return;
	}
	s->child_pid = pid;
}

int session_is_exited(const session_t *s) {
	return s ? s->exited : 0;
}

int session_exit_status(const session_t *s) {
	return s ? s->exit_status : 0;
}

uint64_t session_bytes_in(const session_t *s) {
	return s ? s->bytes_in : 0;
}

uint64_t session_bytes_out(const session_t *s) {
	return s ? s->bytes_out : 0;
}

uint64_t session_input_generation(const session_t *s) {
	return s ? s->input_generation : 0;
}

void session_mark_exited(session_t *s, int status) {
	if (s == NULL || s->exited) {
		return;
	}
	s->exited = 1;
	s->exit_status = status;
	/* The shell is gone; close the master so select() stops waking on the
	 * fd's EOF. session_master_fd() now returns -1 and FD_SET will skip it.
	 * Scrollback in the bridge persists so the user can read the final
	 * output until they dismiss the [exited] tab. */
	if (s->master_fd >= 0) {
		close(s->master_fd);
		s->master_fd = -1;
	}
}

static void record_reply_error(session_t *s, int error) {
	if (!s->reply_error) s->reply_error = error;
}

/* Internal draining never consumes the pending public error notification. */
static int drain_replies(session_t *s) {
	ssize_t n;
	if (s->reply_offset == s->reply_size) return 1;
	if (s->write_exclusive) return 0;
	if (s->master_fd < 0) {
		errno = EBADF;
		n = -1;
	} else {
		n = io_write_master_char(s->master_fd, s->replies + s->reply_offset,
		                         s->reply_size - s->reply_offset);
	}
	if (n > 0 && (size_t)n <= s->reply_size - s->reply_offset) {
		s->bytes_out += (uint64_t)n;
		if ((size_t)n >= s->reply_recovery) s->reply_recovery = 0;
		else s->reply_recovery -= (size_t)n;
		s->reply_offset += (size_t)n;
		if (s->reply_offset != s->reply_size) return 0;
		s->reply_size = s->reply_offset = 0;
		return 1;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return 0;
	if (n >= 0) errno = EIO;
	record_reply_error(s, errno);
	s->reply_size = s->reply_offset = s->reply_recovery = 0;
	return -1;
}

int session_flush_replies(session_t *s) {
	int result;
	if (!s) { errno = EINVAL; return -1; }
	result = drain_replies(s);
	if (s->reply_error) {
		errno = s->reply_error;
		s->reply_error = 0;
		return -1;
	}
	return result;
}

ssize_t session_write_reply(session_t *s, const char *buf, size_t n) {
	size_t pending;
	if (!s || (!buf && n)) { errno = EINVAL; return -1; }
	if (s->master_fd < 0) {
		record_reply_error(s, EBADF);
		errno = EBADF;
		return -1;
	}
	if (!n) return 0;
	/* Try existing replies first; never splice a new response into one that
	 * the PTY has only partly accepted. */
	if (drain_replies(s) < 0) return -1;
	pending = s->reply_size - s->reply_offset;
	if (n > REPLY_CAPACITY - (pending - s->reply_recovery)) {
		record_reply_error(s, ENOBUFS);
		errno = ENOBUFS;
		return -1;
	}
	if (s->reply_offset) memmove(s->replies, s->replies + s->reply_offset, pending);
	memcpy(s->replies + pending, buf, n);
	s->reply_offset = 0;
	s->reply_size = pending + n;
	/* The callback cannot retry EAGAIN. Ownership of accepted bytes remains
	 * with this queue until the event loop drains them or reports a failure. */
	drain_replies(s);
	return (ssize_t)n;
}

static int ready_for_input(session_t *s) {
	int result;
	if (!s || s->master_fd < 0) { errno = EBADF; return -1; }
	if (s->write_exclusive) { errno = EAGAIN; return -1; }
	result = drain_replies(s);
	if (!result) errno = EAGAIN;
	return result > 0 ? 0 : -1;
}

ssize_t session_write_text(session_t *s, const UChar *buf, size_t n) {
	ssize_t w;
	if (ready_for_input(s) < 0) return -1;
	w = io_write_master(s->master_fd, buf, n);
	if (w > 0) { s->bytes_out += (uint64_t)w; ++s->input_generation; }
	return w;
}

ssize_t session_write_bytes(session_t *s, const char *buf, size_t n) {
	if (ready_for_input(s) < 0) return -1;
	return session_write_exclusive(s, buf, n);
}

int session_set_write_exclusive(session_t *s, int enabled) {
	if (!s) { errno = EINVAL; return -1; }
	if (enabled && !s->write_exclusive && ready_for_input(s) < 0) return -1;
	s->write_exclusive = !!enabled;
	return 0;
}

int session_finish_write_exclusive(session_t *s, const char *recovery, size_t n) {
	size_t pending;
	if (!s || !s->write_exclusive || n > RECOVERY_CAPACITY || (!recovery && n)) {
		errno = EINVAL;
		return -1;
	}
	/* Acquiring exclusivity already drained older replies/recovery. Only
	 * automatic replies generated during this paste can now be pending. */
	pending = s->reply_size - s->reply_offset;
	memmove(s->replies + n, s->replies + s->reply_offset, pending);
	if (n) memcpy(s->replies, recovery, n);
	s->reply_size = pending + n;
	s->reply_offset = 0;
	s->reply_recovery = n;
	s->write_exclusive = 0;
	return 0;
}

ssize_t session_write_exclusive(session_t *s, const char *buf, size_t n) {
	ssize_t w;
	if (!s || s->master_fd < 0) { errno = EBADF; return -1; }
	w = io_write_master_char(s->master_fd, buf, n);
	if (w > 0) { s->bytes_out += (uint64_t)w; ++s->input_generation; }
	return w;
}

ssize_t session_read_bytes(session_t *s, char *buf, size_t n) {
	ssize_t r;
	if (s == NULL || s->master_fd < 0) {
		return -1;
	}
	r = io_read_master_raw(s->master_fd, buf, n);
	if (r > 0) s->bytes_in += (uint64_t)r;
	return r;
}

int session_dispatch_action(session_t *s, const action_t *a) {
	if (s == NULL || a == NULL) {
		return 0;
	}
	if(s->write_exclusive){errno=EAGAIN;return 0;}

	switch (a->kind) {
	case TERM_ACTION_SEND_BYTES:
		return send_session_keystrokes(s, a->as.bytes.data);
	case TERM_ACTION_SEND_TERMINFO:
		return send_session_keystrokes(s, a->as.terminfo_name);
	case TERM_ACTION_BUILTIN:
		if (a->as.builtin.id == TERM_BUILTIN_PASTE_CLIPBOARD) {
			char *text = NULL;
			if (s->master_fd >= 0 && is_clipboard_format_present("text/plain") == 0) {
				int length = get_clipboard_data("text/plain", &text);
				if (length > 0 && text) {
					if (!text[length - 1]) --length;
					if (length > 0) session_write_bytes(s, text, (size_t)length);
				}
			}
			free(text);
			return 1;
		}
		return 0;
	}

	return 0;
}
