/* Production session output paths, with a controllable nonblocking PTY and
 * clipboard. Checks wire ordering as well as visible accounting. */
#include "session.h"
#include "io.h"
#include "terminal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char wire[65536];
static size_t wire_size, write_limit = 65536;
static int write_error;
static const char *clipboard = "pasted";

int ghostty_bridge_create(ghostty_bridge_t **out, uint16_t c, uint16_t r,
                         size_t n, void *u) {
	(void)c; (void)r; (void)n; (void)u;
	*out = malloc(1); return *out ? 0 : -1;
}
void ghostty_bridge_destroy(ghostty_bridge_t *bridge) { free(bridge); }
ssize_t io_write_master_char(int fd, const char *s, size_t n) {
	assert(fd >= 0);
	if (write_error) { errno = write_error; return -1; }
	if (n > write_limit) n = write_limit;
	assert(n <= sizeof(wire) - wire_size);
	memcpy(wire + wire_size, s, n); wire_size += n;
	return (ssize_t)n;
}
ssize_t io_write_master(int fd, const UChar *s, size_t n) {
	char bytes[100];
	assert(n <= sizeof(bytes));
	for (size_t i = 0; i < n; ++i) bytes[i] = (char)s[i];
	return io_write_master_char(fd, bytes, n);
}
ssize_t io_read_master_raw(int fd, char *s, size_t n)
{ (void)fd; (void)s; (void)n; errno = EAGAIN; return -1; }
int send_metamode_keystrokes(const char *s) { (void)s; assert(!"must keep the explicit session"); return 0; }
int send_session_keystrokes(session_t *s, const char *k)
{ size_t n = strlen(k); return session_write_bytes(s, k, n) == (ssize_t)n; }
int is_clipboard_format_present(const char *format)
{ assert(!strcmp(format, "text/plain")); return 0; }
int get_clipboard_data(const char *format, char **out) {
	size_t n = strlen(clipboard) + 1;
	assert(!strcmp(format, "text/plain"));
	*out = malloc(n); assert(*out); memcpy(*out, clipboard, n);
	return (int)n;
}

static session_t *fresh(void) {
	session_t *s;
	int fd = open("/dev/null", O_WRONLY);
	assert(fd >= 0 && !session_create(&s, 1, 80, 24, 100));
	session_set_master_fd(s, fd);
	wire_size = 0; write_error = 0; write_limit = sizeof(wire);
	return s;
}
static void equals(const char *expected) {
	assert(wire_size == strlen(expected));
	assert(!memcmp(wire, expected, wire_size));
}

int main(void) {
	session_t *s = fresh();
	UChar typed[] = { 'x' };
	assert(session_write_reply(s, "query", 5) == 5);
	assert(session_bytes_out(s) == 5 && session_input_generation(s) == 0);
	assert(session_write_bytes(s, "k", 1) == 1);
	assert(session_write_text(s, typed, 1) == 1);
	assert(session_input_generation(s) == 2 && session_bytes_out(s) == 7);
	action_t paste = { .kind = TERM_ACTION_BUILTIN };
	paste.as.builtin.id = TERM_BUILTIN_PASTE_CLIPBOARD;
	assert(session_dispatch_action(s, &paste) == 1);
	assert(session_input_generation(s) == 3 && session_bytes_out(s) == 13);
	equals("querykxpasted");
	session_destroy(s);

	/* Session-directed control actions must affect that session's input
	 * generation, never fall back to whichever tab happens to be active. */
	s = fresh();
	session_t *other;
	assert(!session_create(&other, 2, 80, 24, 100));
	action_t keys = { .kind = TERM_ACTION_SEND_BYTES };
	keys.as.bytes.data = "typed";
	assert(session_dispatch_action(s, &keys) == 1);
	assert(session_input_generation(s) == 1 && session_input_generation(other) == 0);
	assert(session_write_reply(s, "reply", 5) == 5);
	assert(session_input_generation(s) == 1);
	assert(!session_set_write_exclusive(s, 1));
	assert(!session_dispatch_action(s, &keys) && session_input_generation(s) == 1);
	equals("typedreply");
	session_destroy(s); session_destroy(other);

	s = fresh();
	assert(!session_set_write_exclusive(s, 1));
	assert(session_write_exclusive(s, "voice:", 6) == 6);
	assert(session_write_reply(s, "first", 5) == 5);
	assert(session_write_reply(s, "second", 6) == 6);
	assert(session_flush_replies(s) == 0);
	equals("voice:");
	assert(session_write_bytes(s, "blocked", 7) == -1 && errno == EAGAIN);
	assert(session_dispatch_action(s, &paste) == 0);
	assert(session_input_generation(s) == 1);
	assert(session_write_exclusive(s, "tail", 4) == 4);
	assert(!session_set_write_exclusive(s, 0));
	write_limit = 3;
	assert(session_flush_replies(s) == 0);
	assert(session_write_bytes(s, "blocked", 7) == -1 && errno == EAGAIN);
	assert(session_flush_replies(s) == 0);
	assert(session_flush_replies(s) == 1);
	equals("voice:tailfirstsecond");
	assert(session_bytes_out(s) == wire_size && session_input_generation(s) == 2);
	session_destroy(s);

	/* Repair a split UTF-8 frame and close bracketed paste before any queued
	 * DA reply is allowed through, even with byte-at-a-time PTY progress. */
	s = fresh();
	assert(!session_set_write_exclusive(s, 1));
	assert(session_write_exclusive(s, "\033[200~\xe4", 7) == 7);
	assert(session_write_reply(s, "\033[?1;2c", 7) == 7);
	assert(session_finish_write_exclusive(s, "too-long-recovery!", 17) == -1 && errno == EINVAL);
	assert(session_write_bytes(s, "blocked", 7) == -1 && errno == EAGAIN);
	assert(session_finish_write_exclusive(s, NULL, 1) == -1 && errno == EINVAL);
	assert(!session_finish_write_exclusive(s, "\xb8\xad\033[201~", 8));
	write_limit = 1;
	while (session_flush_replies(s) == 0) {}
	equals("\033[200~中\033[201~\033[?1;2c");
	assert(session_input_generation(s) == 1 && session_bytes_out(s) == wire_size);
	assert(session_finish_write_exclusive(s, "x", 1) == -1 && errno == EINVAL);
	session_destroy(s);

	/* Full normal queue still leaves all 16 recovery bytes available. It
	 * does not grant normal replies extra capacity while recovery is pending. */
	s = fresh();
	char full[16384]; memset(full, 'r', sizeof(full));
	assert(!session_set_write_exclusive(s, 1));
	assert(session_write_reply(s, full, sizeof(full)) == (ssize_t)sizeof(full));
	assert(!session_finish_write_exclusive(s, "abcdefghijklmnop", 16));
	write_error = EAGAIN;
	assert(session_write_reply(s, "x", 1) == -1 && errno == ENOBUFS);
	assert(session_flush_replies(s) == -1 && errno == ENOBUFS);
	write_error = 0; write_limit = 3;
	assert(session_flush_replies(s) == 0);
	/* Draining only recovery bytes must not free normal-reply capacity. */
	write_error = EAGAIN;
	assert(session_write_reply(s, "x", 1) == -1 && errno == ENOBUFS);
	assert(session_flush_replies(s) == -1 && errno == ENOBUFS);
	write_error = 0; write_limit = sizeof(wire);
	assert(session_flush_replies(s) == 1);
	assert(wire_size == sizeof(full) + 16 && !memcmp(wire, "abcdefghijklmnop", 16));
	assert(!memcmp(wire + 16, full, sizeof(full)));
	assert(session_input_generation(s) == 0 && session_bytes_out(s) == wire_size);
	session_destroy(s);

	/* An earlier response cannot be split by acquiring the voice writer. */
	s = fresh(); write_limit = 1;
	assert(session_write_reply(s, "old", 3) == 3);
	write_error = EAGAIN;
	assert(session_set_write_exclusive(s, 1) == -1 && errno == EAGAIN);
	assert(session_input_generation(s) == 0);
	write_error = 0; write_limit = 100;
	assert(session_write_bytes(s, "new", 3) == 3);
	equals("oldnew");
	assert(session_flush_replies(s) == 1);
	session_destroy(s);

	/* Overflow refuses only the new response and is reported once. */
	s = fresh();
	char large[16384]; memset(large, 'r', sizeof(large));
	assert(!session_set_write_exclusive(s, 1));
	assert(session_write_reply(s, large, sizeof(large)) == (ssize_t)sizeof(large));
	assert(session_write_reply(s, "overflow", 8) == -1 && errno == ENOBUFS);
	assert(session_flush_replies(s) == -1 && errno == ENOBUFS);
	assert(session_flush_replies(s) == 0);
	assert(!session_set_write_exclusive(s, 0));
	assert(session_flush_replies(s) == 1);
	assert(wire_size == sizeof(large) && !memcmp(wire, large, sizeof(large)));
	assert(session_input_generation(s) == 0 && session_bytes_out(s) == wire_size);
	session_destroy(s);

	/* EINTR and EAGAIN retain exactly the unwritten suffix; hard errors do
	 * not replay the prefix and remain observable outside the void callback. */
	s = fresh(); write_limit = 2;
	assert(session_write_reply(s, "abcde", 5) == 5);
	write_error = EINTR; assert(session_flush_replies(s) == 0);
	write_error = EAGAIN; assert(session_flush_replies(s) == 0);
	equals("ab");
	write_error = EIO;
	assert(session_flush_replies(s) == -1 && errno == EIO);
	write_error = 0;
	assert(session_flush_replies(s) == 1);
	assert(session_write_reply(s, "Z", 1) == 1);
	equals("abZ");
	assert(session_input_generation(s) == 0 && session_bytes_out(s) == 3);
	session_destroy(s);

	puts("session output: reply ordering, partial writes, exclusive acquisition, input accounting, overflow and failures PASS");
	return 0;
}
