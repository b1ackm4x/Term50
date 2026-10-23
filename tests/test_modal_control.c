/* Exercise the real termctl client, wire parser and control dispatcher over a
 * Unix socket. Include the TU to expose its static receive path to the test;
 * only BPS/clipboard/window glue is replaced. No user's control socket touched. */
#include <assert.h>
#include <sys/wait.h>
#include "app.h"
#include "terminal.h"
#include "../src/control.c"

static app_t *control_app;
int bps_add_fd(int fd, int events, int (*handler)(int,int,void *), void *data) {
	(void)fd; (void)events; (void)handler; (void)data; return BPS_SUCCESS;
}
int bps_remove_fd(int fd) { (void)fd; return BPS_SUCCESS; }
int is_clipboard_format_present(const char *fmt) { (void)fmt; return -1; }
int get_clipboard_data(const char *fmt, char **data) { (void)fmt; (void)data; return -1; }
int ctl_run_action_string(const char *s) { return app_run_action_string(s); }
const char *ctl_action_error(void) { return "action failed"; }
void ctl_wake(void) {}
int ctl_screen_size(int *cols, int *rows) { *cols=80; *rows=24; return 1; }
unsigned ctl_session_count(void) { return app_session_count(control_app); }
int ctl_tab_stats(unsigned id,char *out,size_t cap) { (void)id; snprintf(out,cap,"test"); return 0; }
int ctl_tabs_stats(char *out,size_t cap) { snprintf(out,cap,"test"); return 1; }
int ctl_notify(const char *a,const char *i,const char *t,const char *b,const char *u,int alert) {
	(void)a; (void)i; (void)t; (void)b; (void)u; (void)alert; return 1;
}
void test_control_roundtrip(app_t *app,const char *client_path) {
	control_app=app;
	const char *commands[]={"mode_normal","mode_insert","mode_toggle","mode_toggle"};
	term_input_mode_t modes[]={TERM_INPUT_NORMAL,TERM_INPUT_INSERT,TERM_INPUT_NORMAL,TERM_INPUT_INSERT};
	struct sockaddr_un addr={0}; addr.sun_family=AF_UNIX;
	char *cwd=getcwd(NULL,0); assert(cwd);
	snprintf(addr.sun_path,sizeof(addr.sun_path),"%s/control.sock",cwd); free(cwd);
	int listener=socket(AF_UNIX,SOCK_STREAM,0); assert(listener>=0);
	assert(bind(listener,(struct sockaddr *)&addr,sizeof(addr))==0); assert(listen(listener,1)==0);
	for (unsigned i=0;i<4;++i) {
		pid_t pid=fork(); assert(pid>=0);
		if(pid==0) {
			setenv(APP_ENV_CONTROL,addr.sun_path,1);
			execl(client_path,client_path,"action",commands[i],(char *)NULL);
			_exit(127);
		}
		int fd=accept(listener,NULL,NULL); assert(fd>=0);
		ctl_client_t c={.fd=fd};
		while(!memchr(c.inbuf,'\n',c.inlen)) {
			ssize_t n=read(fd,c.inbuf+c.inlen,sizeof(c.inbuf)-c.inlen); assert(n>0); c.inlen+=(size_t)n;
		}
		consume_lines(&c);
		close(fd);
		int status; assert(waitpid(pid,&status,0)==pid); assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
		assert(session_input_mode(app_active_session(app))==modes[i]);
	}
	close(listener); unlink(addr.sun_path);
	puts("termctl: 4 real client/control round trips passed");
}
