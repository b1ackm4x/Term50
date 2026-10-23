/* Replace only device IO/render/bridge services. Session, app registry,
 * preferences, Lua, action parser and modal router are production sources. */
#include <stdlib.h>
#include <unistd.h>
#include "ghostty_bridge.h"
#include "io.h"
#include "terminal.h"
#include "platform.h"
int ghostty_bridge_create(ghostty_bridge_t **out, uint16_t c, uint16_t r, size_t n, void *u) {
	(void)c; (void)r; (void)n; (void)u; *out = malloc(1); return *out ? 0 : -1;
}
void ghostty_bridge_destroy(ghostty_bridge_t *b) { free(b); }
ssize_t io_write_master_char(int fd, const char *s, size_t n) { return write(fd, s, n); }
ssize_t io_write_master(int fd, const UChar *s, size_t n) {
	char b[1024]; if (n > sizeof(b)) return -1;
	for (size_t i=0; i<n; ++i) b[i]=(char)s[i];
	return write(fd,b,n);
}
ssize_t io_read_master_raw(int fd, char *s, size_t n) { return read(fd,s,n); }
ssize_t io_read_utf8_string(const char *s, size_t n, UChar *out) {
	for (size_t i=0; i<n; ++i) out[i]=(unsigned char)s[i]; return (ssize_t)n;
}
void io_paste_from_clipboard(int fd) { (void)fd; }
int send_metamode_keystrokes(const char *s) { (void)s; return 0; }
void set_font_size(int n) { (void)n; }
int term_current_font_size(void) { return 24; }
int app_post_notification(const struct notification_spec *s) { (void)s; return 1; }
int term_runtime_ready(void) { return 1; }

int send_session_keystrokes(struct session *s, const char *keys) { (void)s; (void)keys; return 0; }
