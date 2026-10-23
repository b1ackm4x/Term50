#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/keycodes.h>
#include "modal_input.h"
#include "terminal.h"

void test_control_roundtrip(app_t *, const char *);

static app_t *app;
static pref_t *prefs;
static modal_input_t state;
static int checks;
static int dispatches;
static unsigned legacy_mods;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static int dispatch(app_t *a, const action_t *act) {
	++dispatches;
	if (modal_is_mode_action(act)) {
		legacy_mods = state.prefix = 0;
		return modal_apply_action(a, act);
	}
	if (action_is_builtin(act, TERM_BUILTIN_CTRL_DOWN)) { legacy_mods ^= KEYMOD_CTRL; return 1; }
	if (action_is_builtin(act, TERM_BUILTIN_LUA_CALL)) return prefs_lua_invoke(act->as.builtin.arg);
	if (action_is_builtin(act, TERM_BUILTIN_TAB_NEXT)) { app_session_select_next(a); return 1; }
	if (action_is_builtin(act, TERM_BUILTIN_TAB_CLOSE)) { app_session_close_index(a,app_active_index(a)); return 1; }
	return session_dispatch_action(app_active_session(a),act);
}
int app_run_action_string(const char *s) { action_t a; return action_parse(s,&a) && dispatch(app,&a); }
static int event(int sym, int pressed, int repeat, unsigned mods) {
	key_event_t k = {.sym=sym,.keycode=sym,.pressed=pressed,.repeat=repeat,.modifiers=(int)mods};
	int result=modal_input_handle(&state,app,prefs,&k,legacy_mods,dispatch);
	if (result) legacy_mods &= ~state.consumed_mods;
	return result;
}
static int tap(int sym) { int r=event(sym,1,0,0); event(sym,0,0,0); return r; }
static int attach(session_t *s) {
	int fds[2]; CHECK(pipe(fds)==0); CHECK(fcntl(fds[0],F_SETFL,O_NONBLOCK)==0);
	session_set_master_fd(s,fds[1]); return fds[0];
}
static void empty(int fd) { char b; CHECK(read(fd,&b,1)==-1 && errno==EAGAIN); }
static void bytes(int fd, const char *s, size_t n) {
	char b[1024]; ssize_t got=read(fd,b,sizeof(b));
	if (got!=(ssize_t)n) fprintf(stderr,"byte check %d: expected %zu bytes, got %zd\n",checks,n,got);
	CHECK(got==(ssize_t)n); CHECK(!memcmp(b,s,n)); empty(fd);
}
static void config(const char *path,const char *s) { FILE *f=fopen(path,"w"); CHECK(f); CHECK(fputs(s,f)>=0); CHECK(fclose(f)==0); }
static void reload(void) { pref_t *p=prefs_lua_reload(); CHECK(p); destroy_preferences_members(prefs); *prefs=*p; free(p); }
int main(int argc,char **argv) {
	char dir[]="/tmp/term50-modal-XXXXXX"; CHECK(mkdtemp(dir)); CHECK(chdir(dir)==0);
	const char *path=".term.lua";
	config(path, ""); prefs=prefs_lua_load(path); CHECK(prefs);
	if (argc==4 && !strcmp(argv[2],"--emit-reference")) { prefs_emit_lua(prefs,argv[3]); prefs_lua_destroy(prefs); unlink(path); chdir("/"); rmdir(dir); return 0; }
	CHECK(prefs->default_input_mode==TERM_INPUT_INSERT);
	CHECK(app_init(&app,prefs,80,24,100)==0);
	session_t *first=app_active_session(app); int rd=attach(first);
	CHECK(session_input_mode(first)==TERM_INPUT_INSERT);
	CHECK(tap('a')==0); CHECK(tap(' ')==0); CHECK(tap(KEYCODE_ESCAPE)==0);
	CHECK(tap(KEYCODE_BB_SYM_KEY)==0); /* default metamode chord stays on legacy path */
	CHECK(event(KEYCODE_BB_SYM_KEY,1,0,KEYMOD_SHIFT)==0); event(KEYCODE_BB_SYM_KEY,0,0,0);
	CHECK(app_run_action_string("mode_normal"));
	CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	CHECK(!session_set_input_mode(first,(term_input_mode_t)99));
	/* Every unbound printable ASCII byte, plus non-ASCII/special-key truncation. */
	for(int c=32;c<127;++c) if(c!='i' && c!='?') CHECK(tap(c)==1);
	CHECK(tap(0x169)==1); empty(rd);
	/* NORMAL controls operate the terminal without changing mode or leaking
	 * through the INSERT pipeline. Check actual bytes, including NUL. */
	const struct { int key; const char *bytes; } controls[] = {
		{KEYCODE_RETURN,"\r"}, {KEYCODE_KP_ENTER,"\r"}, {KEYCODE_ESCAPE,"\x1b"},
		{KEYCODE_TAB,"\t"}, {KEYCODE_BACK_TAB,"\x1b[Z"}, {KEYCODE_BACKSPACE,"\b"},
		{KEYCODE_DELETE,"\x1b[3~"}, {KEYCODE_INSERT,"\x1b[2~"},
		{KEYCODE_UP,"\x1b[A"}, {KEYCODE_DOWN,"\x1b[B"},
		{KEYCODE_LEFT,"\x1b[D"}, {KEYCODE_RIGHT,"\x1b[C"},
		{KEYCODE_HOME,"\x1b[H"}, {KEYCODE_END,"\x1b[F"},
		{KEYCODE_PG_UP,"\x1b[5~"}, {KEYCODE_PG_DOWN,"\x1b[6~"},
		{13,"\r"}, {27,"\x1b"}, {3,"\x03"}, {127,"\x7f"}
	};
	for (size_t i=0;i<sizeof controls/sizeof controls[0];++i) {
		CHECK(tap(controls[i].key)); bytes(rd,controls[i].bytes,strlen(controls[i].bytes));
		CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	}
	CHECK(event(KEYCODE_TAB,1,0,KEYMOD_SHIFT)); event(KEYCODE_TAB,0,0,0); bytes(rd,"\x1b[Z",3);
	for (int c='a';c<='z';++c) {
		char expected=(char)(c-'a'+1);
		CHECK(event(c,1,0,KEYMOD_CTRL)); event(c,0,0,0); bytes(rd,&expected,1);
		CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	}
	CHECK(event('C',1,0,KEYMOD_CTRL|KEYMOD_SHIFT)); event('C',0,0,0); bytes(rd,"\3",1);
	CHECK(event(' ',1,0,KEYMOD_CTRL)); event(' ',0,0,0); bytes(rd,"\0",1);
	CHECK(event('[',1,0,KEYMOD_CTRL)); event('[',0,0,0); bytes(rd,"\x1b",1);
	/* Sticky physical Ctrl and Q20 Shift -> Alt both produce one Ctrl-C. */
	CHECK(tap(KEYCODE_LEFT_CTRL)); CHECK(tap('c')); bytes(rd,"\3",1);
	CHECK(tap('c')); empty(rd);
	CHECK(tap(KEYCODE_LEFT_SHIFT)); CHECK(tap(KEYCODE_LEFT_ALT)); CHECK(legacy_mods==KEYMOD_CTRL);
	CHECK(tap('c')); bytes(rd,"\3",1); CHECK(legacy_mods==0);
	CHECK(tap('c')); empty(rd);
	/* Holding Enter cannot submit more than once or leak through a mode change. */
	CHECK(event(KEYCODE_RETURN,1,0,0)); bytes(rd,"\r",1);
	CHECK(event(KEYCODE_RETURN,1,1,0)); empty(rd);
	CHECK(app_run_action_string("mode_insert")); CHECK(event(KEYCODE_RETURN,1,1,0)); empty(rd);
	event(KEYCODE_RETURN,0,0,0); CHECK(tap(KEYCODE_RETURN)==0);
	CHECK(app_run_action_string("mode_normal"));
	CHECK(tap('i')==1); CHECK(session_input_mode(first)==TERM_INPUT_INSERT); empty(rd);
	CHECK(app_run_action_string("mode_normal"));
	CHECK(event(' ',1,0,0)); empty(rd);
	CHECK(event(' ',1,1,0)); CHECK(session_input_mode(first)==TERM_INPUT_INSERT);
	for(int i=0;i<8;++i) CHECK(event(' ',1,1,0));
	CHECK(event(' ',0,0,0)); empty(rd); CHECK(tap(' ')==0);
	CHECK(app_run_action_string("mode_normal"));
	CHECK(event('i',1,0,0)); CHECK(event('i',1,1,0)); empty(rd); event('i',0,0,0);
	config(path,
	 "default_input_mode='normal'\n"
	 "normal_keys={{'g','prefix ',next_mode='insert',label='Prompt'},"
	 "{'x','\\x03'},{'r','line\\r'},{'m','lua:bad',next_mode='insert'},"
	 "{'z','mode_insert',next_mode='normal'},{'l','tab_next',next_mode='insert'},"
	 "{'q','tab_close',next_mode='normal'},{'i',false}}\n"
	 "function bad() error('expected failure') end\n"
	 "chord_bindings={{key='sym',mods={'shift'},action='mode_toggle'}}\n");
	reload(); CHECK(session_input_mode(first)==TERM_INPUT_INSERT); CHECK(prefs->default_input_mode==TERM_INPUT_NORMAL);
	CHECK(!keymap_lookup('i',prefs->normal_keys)); CHECK(!strcmp(keymap_lookup('g',prefs->normal_keys)->label,"Prompt"));
	/* Real modifier bits, then the NORMAL tap-to-stick Shift path. */
	CHECK(event(KEYCODE_BB_SYM_KEY,1,0,KEYMOD_SHIFT)); event(KEYCODE_BB_SYM_KEY,0,0,0);
	CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	CHECK(tap(KEYCODE_LEFT_SHIFT)); CHECK(tap(KEYCODE_BB_SYM_KEY)); CHECK(session_input_mode(first)==TERM_INPUT_INSERT);
	CHECK(app_run_action_string("mode_normal"));
	CHECK(tap('g')); bytes(rd,"prefix ",7); CHECK(session_input_mode(first)==TERM_INPUT_INSERT);
	CHECK(app_run_action_string("mode_normal")); CHECK(tap('x')); bytes(rd,"\3",1); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	CHECK(tap('r')); bytes(rd,"line\r",5); CHECK(tap('m')); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	CHECK(tap('z')); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL); empty(rd);
	/* Failed write cannot apply next_mode. */
	int oldfd=session_master_fd(first); session_set_master_fd(first,-1);
	CHECK(tap('g')); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL); session_set_master_fd(first,oldfd);
	session_t *second; CHECK(app_session_open(app,80,24,100,&second)==0); CHECK(session_input_mode(second)==TERM_INPUT_NORMAL);
	CHECK(app_run_action_string("mode_insert"));
	CHECK(app_session_select_index(app,0)); CHECK(session_input_mode(app_active_session(app))==TERM_INPUT_NORMAL);
	CHECK(tap('l')); CHECK(app_active_session(app)==second); CHECK(session_input_mode(first)==TERM_INPUT_INSERT); CHECK(session_input_mode(second)==TERM_INPUT_INSERT);
	CHECK(app_session_select_index(app,0)); CHECK(app_run_action_string("mode_normal"));
	CHECK(event(' ',1,0,0)); CHECK(app_session_select_index(app,1)); CHECK(event(' ',1,1,0)); CHECK(session_input_mode(second)==TERM_INPUT_INSERT); event(' ',0,0,0);
	/* Lua term.action uses the same parser + mode dispatcher. */
	char out[256]; CHECK(prefs_lua_eval("return term.action('mode_normal')",out,sizeof(out))==0); CHECK(!strcmp(out,"true"));
	CHECK(session_input_mode(second)==TERM_INPUT_NORMAL);
	CHECK(tap('q')); CHECK(app_session_count(app)==1); CHECK(app_active_session(app)==first); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	config(path,"normal_keys={{'g','updated',next_mode='insert'}}\n"); reload(); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL); CHECK(prefs->default_input_mode==TERM_INPUT_INSERT);
	CHECK(tap('g')); bytes(rd,"updated",7); CHECK(session_input_mode(first)==TERM_INPUT_INSERT);
	CHECK(app_session_open(app,80,24,100,&second)==0); CHECK(session_input_mode(second)==TERM_INPUT_INSERT);
	config(path,"this is invalid Lua !!!"); CHECK(!prefs_lua_reload()); CHECK(session_input_mode(first)==TERM_INPUT_INSERT); CHECK(!strcmp(keymap_lookup('g',prefs->normal_keys)->to,"updated"));
	config(path,"normal_keys={{'i','mode_normal',next_mode='bogus'}}"); reload(); CHECK(action_is_builtin(&keymap_lookup('i',prefs->normal_keys)->action,TERM_BUILTIN_MODE_INSERT));
	/* Override/unbind/re-add in one table must free each label exactly once. */
	config(path,"normal_keys={{'a','first',label='old'},{'b','second',label='keep'},{'a',false},{'c','third'},{'b','new',label='new'}}"); reload();
	CHECK(!keymap_lookup('a',prefs->normal_keys)); CHECK(!keymap_lookup('c',prefs->normal_keys)->label);
	CHECK(!strcmp(keymap_lookup('b',prefs->normal_keys)->label,"new"));
	CHECK(app_session_select_index(app,0)); CHECK(app_run_action_string("mode_normal"));
	/* Disable hold and use a custom hold trigger without changing INSERT. */
	prefs->normal_insert_hold_enabled=0;
	CHECK(event(' ',1,0,0)); CHECK(event(' ',1,1,0)); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL); event(' ',0,0,0); empty(rd);
	prefs->normal_insert_hold_enabled=1; prefs->normal_insert_hold_key='f';
	CHECK(event('f',1,0,0)); CHECK(event('f',1,1,0)); CHECK(session_input_mode(first)==TERM_INPUT_INSERT); event('f',0,0,0); empty(rd);
	/* Prefix chords in INSERT are supplied by the unchanged sticky-key layer. */
	config(path,"chord_bindings={{key='sym',mods={'shift'},action='mode_toggle'}}"); reload();
	key_event_t sym={.sym=KEYCODE_BB_SYM_KEY,.pressed=1};
	CHECK(modal_input_handle(&state,app,prefs,&sym,KEYMOD_SHIFT,dispatch)); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL);
	CHECK(event(KEYCODE_BB_SYM_KEY,1,1,KEYMOD_SHIFT)); CHECK(session_input_mode(first)==TERM_INPUT_NORMAL); event(KEYCODE_BB_SYM_KEY,0,0,0);
	/* An explicit chord still overrides the default terminal operation. */
	config(path,"chord_bindings={{key='enter',mods={},action='mode_insert'},{key='c',mods={'ctrl'},action='mode_insert'}}"); reload();
	CHECK(event('c',1,0,KEYMOD_CTRL)); event('c',0,0,0); CHECK(session_input_mode(first)==TERM_INPUT_INSERT); empty(rd);
	CHECK(app_run_action_string("mode_normal"));
	CHECK(tap(KEYCODE_RETURN)); CHECK(session_input_mode(first)==TERM_INPUT_INSERT); empty(rd);
	config(path,"chord_bindings={{key='sym',mods={'shift'},action='mode_toggle'}}"); reload();
	CHECK(app_run_action_string("mode_normal"));
	/* Alt+v resolves the configured '?' glyph, not the base letter. */
	int before=dispatches; CHECK(tap(KEYCODE_BB_ALT_KEY)); CHECK(tap('v')); CHECK(dispatches==before+1); empty(rd);
	/* Closing a tab before the active one preserves active identity + mode. */
	session_t *third; CHECK(app_session_open(app,80,24,100,&third)==0);
	CHECK(app_session_select_index(app,1)); CHECK(app_run_action_string("mode_normal"));
	app_session_close_index(app,0); CHECK(app_active_session(app)==second); CHECK(session_input_mode(second)==TERM_INPUT_NORMAL);
	CHECK(app_session_select_index(app,1)); CHECK(app_active_session(app)==third); CHECK(session_input_mode(third)==TERM_INPUT_INSERT);
	/* Full keyboard with no key-up events cannot exhaust suppression slots. */
	CHECK(app_run_action_string("mode_normal"));
	for (int c=33; c<127; ++c) if(c!='i' && c!='?') CHECK(event(c,1,0,0));
	CHECK(event(' ',1,0,0)); CHECK(event(' ',1,1,0)); CHECK(session_input_mode(third)==TERM_INPUT_INSERT);
	CHECK(event(' ',1,1,0)); event(' ',0,0,0);
	/* A runtime error during reload is as non-destructive as a syntax error. */
	config(path,"default_input_mode='normal'; error('expected reload failure')");
	CHECK(!prefs_lua_reload()); CHECK(prefs->default_input_mode==TERM_INPUT_INSERT);
	CHECK(session_input_mode(third)==TERM_INPUT_INSERT);
	/* Ship the actual example, not a separate copy of its product commands. */
	FILE *example=fopen(getenv("TERM50_TEST_EXAMPLE"),"r"); CHECK(example);
	char example_text[8192]; size_t example_n=fread(example_text,1,sizeof(example_text)-1,example); CHECK(!ferror(example)); fclose(example); example_text[example_n]=0;
	config(path,example_text); reload(); CHECK(prefs->default_input_mode==TERM_INPUT_NORMAL);
	CHECK(keymap_lookup('g',prefs->normal_keys)->next_mode==1);
	CHECK(keymap_lookup('r',prefs->normal_keys)->next_mode==0);
	keymap_t *exit_key=keymap_lookup('q',prefs->normal_keys); CHECK(exit_key && !strchr(exit_key->to,'\r'));
	CHECK(!strcmp(keymap_lookup('x',prefs->normal_keys)->to,"\x03"));
	chord_t *esc=chord_lookup(KEYCODE_BB_SYM_KEY,KEYMOD_ALT,prefs->chord_bindings);
	CHECK(esc && esc->action.kind==TERM_ACTION_SEND_BYTES && esc->action.as.bytes.len==1 && esc->action.as.bytes.data[0]==0x1b);
	CHECK(event(KEYCODE_BB_SYM_KEY,1,0,KEYMOD_ALT)==0); event(KEYCODE_BB_SYM_KEY,0,0,0);
	prefs_emit_lua(prefs,"roundtrip.lua"); CHECK(rename("roundtrip.lua",path)==0); reload();
	CHECK(prefs->default_input_mode==TERM_INPUT_NORMAL);
	CHECK(keymap_lookup('g',prefs->normal_keys)->next_mode==1);
	CHECK(keymap_lookup('g',prefs->normal_keys)->label!=NULL);
	CHECK(argc>=2); test_control_roundtrip(app,argv[1]);
	/* This exact change detector feeds main's full-repaint gate. */
	modal_view_t view={0};
	CHECK(modal_view_update(&view,app)); CHECK(view.mode==TERM_INPUT_INSERT);
	CHECK(!modal_view_update(&view,app));
	CHECK(app_run_action_string("mode_normal")); CHECK(modal_view_update(&view,app)); CHECK(view.mode==TERM_INPUT_NORMAL);
	CHECK(app_session_select_index(app,0)); CHECK(modal_view_update(&view,app)); CHECK(view.session==session_id(second)); CHECK(view.mode==TERM_INPUT_NORMAL);
	CHECK(!modal_view_update(&view,app));
	CHECK(app_run_action_string("mode_insert")); CHECK(modal_view_update(&view,app)); CHECK(view.mode==TERM_INPUT_INSERT);
	CHECK(dispatches>10);
	/* Profile keymaps are independent and N/I toggles restore the last
	 * selected operation profile instead of overwriting the user's bindings. */
	config(path,"normal_keys={{'m','CLAUDE'}}; terminal_keys={{'m','TERMINAL'}}"); reload();
	session_t *profile_tab; CHECK(app_session_open(app,80,24,100,&profile_tab)==0);
	int profile_rd=attach(profile_tab); memset(&state,0,sizeof(state)); legacy_mods=0;
	CHECK(session_profile(profile_tab)==TERM_PROFILE_TTY);
	CHECK(app_run_action_string("profile_claude")); CHECK(session_profile(profile_tab)==TERM_PROFILE_CLAUDE);
	CHECK(tap('m')); bytes(profile_rd,"CLAUDE",6);
	CHECK(app_run_action_string("profile_terminal")); CHECK(session_profile(profile_tab)==TERM_PROFILE_TERMINAL);
	CHECK(tap('m')); bytes(profile_rd,"TERMINAL",8);
	CHECK(tap('r')); bytes(profile_rd,"\x12",1);
	CHECK(session_profile(profile_tab)==TERM_PROFILE_TERMINAL);
	CHECK(!strcmp(keymap_lookup('h',prefs->terminal_keys)->to,"tab_prev"));
	CHECK(app_run_action_string("profile_claude")); CHECK(tap('r')); empty(profile_rd);
	CHECK(app_run_action_string("profile_terminal"));
	CHECK(app_run_action_string("mode_toggle")); CHECK(session_profile(profile_tab)==TERM_PROFILE_TTY);
	CHECK(app_run_action_string("mode_toggle")); CHECK(session_profile(profile_tab)==TERM_PROFILE_TERMINAL);
	CHECK(!session_set_profile(profile_tab,(term_profile_t)999));
	CHECK(app_run_action_string("profile_tty")); CHECK(tap('m')==0); empty(profile_rd);
	CHECK(tap('r')==0); empty(profile_rd);
	CHECK(session_profile(second)==TERM_PROFILE_TTY);
	CHECK(keymap_lookup('i',prefs->terminal_keys));
	CHECK(!strcmp(keymap_lookup('i',prefs->terminal_keys)->to,"kcuu1"));
	close(profile_rd);
	app_shutdown_state(app); prefs_lua_destroy(prefs); close(rd); unlink(path); chdir("/"); rmdir(dir);
	printf("modal input: %d checks passed\n",checks); return 0;
}
