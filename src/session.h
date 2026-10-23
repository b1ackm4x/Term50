/*
 * A Term50 session: one pty/child + terminal emulator + IO + scrollback.
 *
 * The app owns a session set. Each tab preserves its own input mode. `session_t` owns its ghostty_bridge_t, the pty
 * master fd, and the child shell's pid + exit state.
 */

#ifndef SESSION_H_
#define SESSION_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unicode/utf.h>

#include "term_types.h"
#include "action.h"

#include "ghostty_bridge.h"
typedef struct session session_t;

/* Owns its ghostty_bridge_t, constructed at the given geometry / scrollback.
 * The pty master fd is adopted via session_set_master_fd() after the caller
 * runs openpty()/forkpty(); session_destroy closes it. The child pid is
 * adopted via session_set_child_pid() and the SIGCHLD reaper marks the
 * session exited; the bridge keeps its scrollback so the user can read
 * the shell's last output until they dismiss the [exited] tab. */
int  session_create(session_t **out, session_id_t id,
                    uint16_t cols, uint16_t rows, size_t max_scrollback);
void session_destroy(session_t *s);                 /* NULL-safe */

term_input_mode_t session_input_mode(const session_t *s);
/* Returns 1 on change, 0 on no change/invalid input. */
int session_set_input_mode(session_t *s, term_input_mode_t mode);
term_profile_t session_profile(const session_t *s);
int session_set_profile(session_t *s, term_profile_t profile);

session_id_t      session_id(const session_t *s);
ghostty_bridge_t *session_bridge(session_t *s);
int               session_master_fd(const session_t *s);
void              session_set_master_fd(session_t *s, int fd);

pid_t session_child_pid(const session_t *s);
void  session_set_child_pid(session_t *s, pid_t pid);
int   session_is_exited(const session_t *s);
int   session_exit_status(const session_t *s);    /* waitpid status, valid when is_exited */
void  session_mark_exited(session_t *s, int status);

/* Cumulative pty byte counters, maintained by the read/write helpers below.
 * "in" = bytes read from the shell; "out" = bytes written to the shell. */
uint64_t session_bytes_in(const session_t *s);
uint64_t session_bytes_out(const session_t *s);
/* Changes after successful user-input writes, including clipboard and voice.
 * Automatic terminal query replies do not invalidate a composition draft. */
uint64_t session_input_generation(const session_t *s);

ssize_t session_write_text(session_t *s, const UChar *buf, size_t n);
ssize_t session_write_bytes(session_t *s, const char *buf, size_t n);
/* A literal paste owns output until drained; unrelated writes fail EAGAIN.
 * Acquire returns 0 on success, -1 if an older reply is still pending (EAGAIN)
 * or the PTY failed. Release returns 0. Check acquisition before writing. */
int session_set_write_exclusive(session_t *s, int enabled);
/* End an interrupted literal paste: queue up to 16 recovery bytes ahead of
 * every deferred automatic reply, then release exclusivity. Reserved capacity
 * makes this possible even when the normal 16 KiB queue is full. Returns 0,
 * or -1/EINVAL without releasing exclusivity for invalid arguments/state. */
int session_finish_write_exclusive(session_t *s, const char *recovery, size_t n);
ssize_t session_write_exclusive(session_t *s, const char *buf, size_t n);
/* Automatic query replies use a bounded 16 KiB queue. Returns bytes accepted,
 * including queued bytes; -1 rejects this whole reply. No user generation
 * change. The event loop must call flush even when no new PTY output arrives. */
ssize_t session_write_reply(session_t *s, const char *buf, size_t n);
/* 1 empty, 0 pending/exclusive/would-block, -1 reports a reply error once via
 * errno. Overflow rejects only the new reply; hard write errors discard the
 * unsendable queue. Neither condition replays previously written bytes. */
int session_flush_replies(session_t *s);
ssize_t session_read_bytes(session_t *s, char *buf, size_t n);

/* Session-scoped action subset: SEND_BYTES / SEND_TERMINFO / PASTE_CLIPBOARD.
 * Returns 1 if handled, 0 otherwise. NULL session => 0. */
int  session_dispatch_action(session_t *s, const action_t *a);

#endif /* SESSION_H_ */
