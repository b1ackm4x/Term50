#!/usr/bin/env python3
"""Host regression of production voice coordination, without QNX/audio/network.

Only main.c's contiguous voice functions and globals are extracted, with strict
boundary/name assertions. They link the real quick_ui/session/voice_commit code.
Platform services, client snapshots and PTY acceptance are controlled fakes.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source, first, after):
    assert source.count(first) == 1, f"ambiguous/missing source boundary: {first}"
    assert source.count(after) == 1, f"ambiguous/missing source boundary: {after}"
    start, end = source.index(first), source.index(after)
    assert start < end, "production extraction boundaries reversed"
    return source[start:end]


PRELUDE = r'''
#include "quick_ui.h"
#include "voice_commit.h"
#include "session.h"
#include "io.h"
#include <sys/keycodes.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
typedef struct { session_t *slots[2]; unsigned active; } app_t;
static app_t fake_app, *g_app=&fake_app;
static void *g_platform;
static struct { const char *tty_encoding,*font_path; int screen_idle_awake; } fake_prefs={"UTF-8","test-font.ttf",0}, *prefs=&fake_prefs;
static quick_ui_t g_ui;
typedef struct { unsigned completions; } ui_painter_t;
static ui_painter_t g_ui_painter;
static unsigned prepare_calls,consumed_prepares;
static int ui_painter_voice_prepare(ui_painter_t *p,quick_ui_t *u,const char *path){
    CHECK(p==&g_ui_painter&&u==&g_ui&&path==prefs->font_path);++prepare_calls;return 0;
}
static uint64_t voice_shortcut_now(void){return 1234;}
static void ui_painter_voice_complete(ui_painter_t *p,uint64_t now){CHECK(p==&g_ui_painter&&now==1234);++p->completions;}
static char g_ui_settings_dir[1];
static int current_help_overlay;
#define PLATFORM_IDLE_TIMEOUT_MS_DEFAULT 250
struct voice_client { voice_snapshot_t snap; };
static struct voice_client fake_client;
static int starts, stops, cancels, last_probe, start_failure, clipboard_failure;
static size_t write_limit;
static int write_error, bracketed=1;
static char wire[2][65536], clipboard[VOICE_TEXT_MAX+1], toast[256];
static size_t wire_size[2];
static session_t *app_session_by_id(app_t *a,session_id_t id){
    for(int i=0;i<2;i++)if(a->slots[i]&&session_id(a->slots[i])==id)return a->slots[i];
    return NULL;
}
static session_t *app_active_session(app_t *a){return a->slots[a->active];}
static int app_session_index_of(app_t *a,session_t *s,unsigned *n){
    for(unsigned i=0;i<2;i++)if(a->slots[i]==s){*n=i;return 1;}return 0;
}
static int app_session_select_index(app_t *a,unsigned n){a->active=n;return 1;}
static void app_dispatch_action(app_t *a,const action_t *b){(void)a;(void)b;CHECK(0);}
static void tab_overlay_set(int n){(void)n;}
static void help_overlay_build(void){}
static void mark_screen_dirty(int n){(void)n;}
static void platform_toast(void *p,const char *s){(void)p;snprintf(toast,sizeof(toast),"%s",s);}
static unsigned awake_calls;
static int awake_value;
static int platform_set_screen_awake(void *p,int n){CHECK(p==g_platform&&(n==0||n==1));++awake_calls;awake_value=n;return 0;}
static void platform_set_idle_timeout(void *p,int n){(void)p;(void)n;}
static void ui_sync(void){
    ui_tab_t tabs[2];unsigned n=0;
    for(int i=0;i<2;i++)if(fake_app.slots[i])tabs[n++]=(ui_tab_t){session_id(fake_app.slots[i]),TERM_INPUT_INSERT,TERM_PROFILE_TTY};
    quick_ui_sync(&g_ui,tabs,n,session_id(app_active_session(g_app)));
}
int ghostty_bridge_create(ghostty_bridge_t **out,uint16_t c,uint16_t r,size_t n,void *u){
    (void)c;(void)r;(void)n;(void)u;*out=malloc(1);return *out?0:-1;
}
void ghostty_bridge_destroy(ghostty_bridge_t *b){free(b);}
int ghostty_bridge_bracketed_paste(ghostty_bridge_t *b){CHECK(b);return bracketed;}
ssize_t io_write_master_char(int fd,const char *s,size_t n){
    int slot=-1;for(int i=0;i<2;i++)if(fake_app.slots[i]&&session_master_fd(fake_app.slots[i])==fd)slot=i;
    CHECK(slot>=0);if(write_error){errno=write_error;return -1;}
    if(n>write_limit)n=write_limit;CHECK(n<=sizeof(wire[slot])-wire_size[slot]);
    memcpy(wire[slot]+wire_size[slot],s,n);wire_size[slot]+=n;return (ssize_t)n;
}
ssize_t io_write_master(int fd,const UChar *s,size_t n){
    char bytes[100];CHECK(n<=sizeof(bytes));for(size_t i=0;i<n;i++)bytes[i]=(char)s[i];return io_write_master_char(fd,bytes,n);
}
ssize_t io_read_master_raw(int fd,char *s,size_t n){(void)fd;(void)s;(void)n;errno=EAGAIN;return -1;}
int io_copy_to_clipboard(const char *s,size_t n){
    if(clipboard_failure)return -1;CHECK(n<sizeof(clipboard));memcpy(clipboard,s,n);clipboard[n]=0;return 0;
}
int is_clipboard_format_present(const char *s){(void)s;return 0;}
int get_clipboard_data(const char *format,char **out){CHECK(!strcmp(format,"text/plain"));*out=strdup(clipboard);CHECK(*out);return (int)strlen(*out)+1;}
int send_metamode_keystrokes(const char *s){(void)s;CHECK(0);return 0;}
int send_session_keystrokes(session_t *s,const char *k){size_t n=strlen(k);return session_write_bytes(s,k,n)==(ssize_t)n;}
voice_client_t *voice_client_create(void){return &fake_client;}
int voice_client_start(voice_client_t *v,const char *host,int port,const char *key,int probe){
    CHECK(v==&fake_client&&host&&key&&port>0);
    if(!probe){CHECK(prepare_calls>consumed_prepares);consumed_prepares=prepare_calls;}
    if(start_failure)return -1;
    ++starts;last_probe=probe;unsigned change=v->snap.change+1;memset(&v->snap,0,sizeof(v->snap));v->snap.change=change;v->snap.state=VOICE_CONNECTING;return 0;
}
void voice_client_stop(voice_client_t *v){CHECK(v);++stops;v->snap.state=VOICE_FINALIZING;++v->snap.change;}
void voice_client_cancel(voice_client_t *v){if(v){++cancels;v->snap.state=VOICE_IDLE;++v->snap.change;}}
void voice_client_snapshot(voice_client_t *v,voice_snapshot_t *s){CHECK(v);*s=v->snap;}
void voice_client_destroy(voice_client_t *v){(void)v;}
'''


TESTS = r'''
static void fresh(void){
    for(int i=0;i<2;i++){session_destroy(fake_app.slots[i]);fake_app.slots[i]=NULL;}
    memset(&fake_client,0,sizeof(fake_client));memset(wire,0,sizeof(wire));memset(wire_size,0,sizeof(wire_size));
    memset(clipboard,0,sizeof(clipboard));toast[0]=0;memset(&g_ui_painter,0,sizeof(g_ui_painter));
    prepare_calls=consumed_prepares=0;
    starts=stops=cancels=last_probe=start_failure=clipboard_failure=write_error=0;write_limit=sizeof(wire[0]);bracketed=1;
    g_voice=NULL;g_voice_target=g_voice_probe_target=g_voice_commit_target=0;
    g_voice_change=0;g_voice_probe=g_voice_committing=g_reply_waiting=0;g_voice_original_out=0;
    memset(&g_voice_commit,0,sizeof(g_voice_commit));memset(g_voice_commit_text,0,sizeof(g_voice_commit_text));
    fake_app.active=0;prefs->tty_encoding="UTF-8";prefs->screen_idle_awake=0;
    g_voice_screen_awake=0;awake_calls=0;awake_value=-1;
    for(unsigned i=0;i<2;i++){
        CHECK(!session_create(&fake_app.slots[i],11+i,80,24,100));
        int fd=open("/dev/null",O_WRONLY);CHECK(fd>=0);session_set_master_fd(fake_app.slots[i],fd);
    }
    ui_settings_t s;ui_settings_defaults(&s);s.companion_enabled=1;strcpy(s.companion_host,"192.0.2.1");memset(s.companion_token,'1',64);s.companion_token[64]=0;
    quick_ui_init(&g_ui,&s);quick_ui_geometry(&g_ui,720,720,0);ui_sync();quick_ui_open(&g_ui,UI_VOICE);
}
static void key_only(int sym){
    event_t e={.type=TERM_EVENT_KEY};e.as.key.sym=sym;e.as.key.pressed=1;CHECK(quick_ui_event(&g_ui,&e));
    e.as.key.pressed=0;CHECK(quick_ui_event(&g_ui,&e));
}
static void press(int sym){key_only(sym);ui_apply_effect();}
static void click(const char *label){
    int found=-1;for(int i=0;i<g_ui.item_count;i++)if(!strcmp(g_ui.items[i].label,label)){found=i;break;}CHECK(found>=0);
    rect_t r=g_ui.items[found].rect;event_t e={.type=TERM_EVENT_TOUCH_DOWN};e.as.touch.x=r.x+r.w/2;e.as.touch.y=r.y+r.h/2;
    CHECK(quick_ui_event(&g_ui,&e));e.type=TERM_EVENT_TOUCH_UP;CHECK(quick_ui_event(&g_ui,&e));ui_apply_effect();
}
static void expanded(void){
    if(!quick_ui_voice_inline(&g_ui))return;
    const char *label=g_ui.voice_state==UI_VOICE_READY?"修改":g_ui.voice_state==UI_VOICE_ERROR?"详情":"展开";
    click(label);CHECK(!quick_ui_voice_inline(&g_ui)&&g_ui.voice_expanded);
}
static void copy_draft(void){click(quick_ui_voice_inline(&g_ui)?"复制":"复制草稿");}
static void publish(int state,unsigned revision,const char *text){
    fake_client.snap.state=state;fake_client.snap.revision=revision;snprintf(fake_client.snap.text,sizeof(fake_client.snap.text),"%s",text);++fake_client.snap.change;voice_tick();
}
static void ready(const char *text){
    size_t before[2]={wire_size[0],wire_size[1]};
    click("开始录音");CHECK(starts==1&&!last_probe&&g_voice_target==11&&prepare_calls==1);voice_tick();
    publish(VOICE_RECORDING,1,"部分草稿");CHECK(g_ui.voice_state==UI_VOICE_RECORDING);
    CHECK(wire_size[0]==before[0]&&wire_size[1]==before[1]);
    press(KEYCODE_RETURN);CHECK(stops==1&&g_ui.voice_state==UI_VOICE_FINALIZING);
    CHECK(wire_size[0]==before[0]&&wire_size[1]==before[1]);
    publish(VOICE_READY,2,text);CHECK(g_ui.voice_state==UI_VOICE_READY&&!strcmp(g_ui.voice_text,text));
    CHECK(wire_size[0]==before[0]&&wire_size[1]==before[1]);
}
static void health(void){
    expanded();click("Mac 连接");voice_tick();CHECK(g_voice_target==11&&g_ui.voice_state==UI_VOICE_READY);
    unsigned prepared=prepare_calls;click("测试连接");CHECK(g_voice_probe&&g_voice_probe_target==11&&last_probe);CHECK(prepare_calls==prepared);
    publish(VOICE_READY,0,"");CHECK(!g_voice_probe&&g_voice_target==11&&g_ui.voice_state==UI_VOICE_READY);
    press(KEYCODE_ESCAPE);CHECK(g_ui.page==UI_VOICE);
}
static void drain(void){
    unsigned turns=0;while(g_voice_committing){CHECK(++turns<10000);voice_tick();}
    turns=0;while(session_flush_replies(fake_app.slots[0])==0)CHECK(++turns<10000);
}
static void wire_equals(int slot,const char *expected){CHECK(wire_size[slot]==strlen(expected));CHECK(!memcmp(wire[slot],expected,wire_size[slot]));}
int main(void){
    CHECK(strstr(voice_error_status("CLIENT_TIMEOUT"),"超时"));
    CHECK(strstr(voice_error_status("FINAL_TIMEOUT"),"超时"));
    CHECK(strstr(voice_error_status("BRIDGE_START_TIMEOUT"),"超时"));
    CHECK(strstr(voice_error_status("response_timeout"),"超时"));
    CHECK(strstr(voice_error_status("MICROPHONE_ROUTE_PAGE_TIMEOUT"),"语音输入页"));
    CHECK(strstr(voice_error_status("MICROPHONE_ROUTE_TIMEOUT"),"超时"));
    CHECK(strstr(voice_error_status("MICROPHONE_ROUTE_VERIFY_TIMEOUT"),"超时"));
    CHECK(strstr(voice_error_status("FOREGROUND_RESTORE_FAILED"),"原窗口"));
    /* A temporary window policy covers connect/capture/finalization only.
     * Repeated pump turns do not issue native calls, and every terminal
     * outcome restores the user's original screen-idle preference. */
    fresh();voice_screen_awake_sync();CHECK(!awake_calls&&!g_voice_screen_awake);
    click("开始录音");CHECK(g_ui.voice_state==UI_VOICE_CONNECTING);voice_screen_awake_sync();
    CHECK(awake_calls==1&&awake_value==1&&g_voice_screen_awake);
    for(int i=0;i<20;i++)voice_screen_awake_sync();CHECK(awake_calls==1);
    publish(VOICE_RECORDING,1,"部分稿");voice_screen_awake_sync();CHECK(awake_calls==1&&awake_value==1);
    press(KEYCODE_RETURN);CHECK(g_ui.voice_state==UI_VOICE_FINALIZING);voice_screen_awake_sync();CHECK(awake_calls==1);
    publish(VOICE_READY,2,"完成稿");voice_screen_awake_sync();CHECK(awake_calls==2&&awake_value==0&&!g_voice_screen_awake);
    for(int i=0;i<20;i++)voice_screen_awake_sync();CHECK(awake_calls==2);
    press(KEYCODE_ESCAPE);voice_screen_awake_sync();CHECK(awake_calls==2);

    fresh();click("开始录音");voice_screen_awake_sync();press(KEYCODE_ESCAPE);voice_screen_awake_sync();
    CHECK(!g_voice_target&&!g_voice_screen_awake&&awake_calls==2&&awake_value==0);wire_equals(0,"");
    fresh();click("开始录音");voice_screen_awake_sync();fake_app.active=1;ui_sync();voice_tick();voice_screen_awake_sync();
    CHECK(!g_voice_target&&g_ui.voice_state==UI_VOICE_ERROR&&!g_voice_screen_awake&&awake_calls==2&&awake_value==0);
    fresh();click("开始录音");voice_screen_awake_sync();publish(VOICE_ERROR,0,"");voice_screen_awake_sync();
    CHECK(g_ui.voice_state==UI_VOICE_ERROR&&!g_voice_screen_awake&&awake_calls==2&&awake_value==0);

    fresh();prefs->screen_idle_awake=1;awake_value=1;voice_screen_awake_sync();CHECK(!awake_calls&&awake_value==1);
    click("开始录音");voice_screen_awake_sync();publish(VOICE_RECORDING,1,"偏好长亮");voice_screen_awake_sync();
    CHECK(awake_calls==1&&awake_value==1&&g_voice_screen_awake);
    press(KEYCODE_RETURN);voice_screen_awake_sync();publish(VOICE_READY,2,"最终稿");voice_screen_awake_sync();
    CHECK(awake_calls==2&&awake_value==1&&!g_voice_screen_awake);
    voice_screen_awake_sync();CHECK(awake_calls==2);
    fresh();quick_ui_open(&g_ui,UI_CONNECTION);click("测试连接");voice_screen_awake_sync();CHECK(!g_voice_target&&!awake_calls);
    press(KEYCODE_ESCAPE);voice_tick();voice_screen_awake_sync();CHECK(!awake_calls);

    /* Default voice entry stays compact; every streamed snapshot is pre-edit only.
     * Cancel must discard the composition without a success feedback or PTY write. */
    fresh();CHECK(quick_ui_voice_inline(&g_ui));CHECK(!g_ui.voice_rect.w&&!g_ui.voice_rect.h);
    CHECK(g_ui.voice_capsule.w<g_ui.w&&g_ui.voice_capsule.h>=48);
    click("开始录音");wire_equals(0,"");wire_equals(1,"");
    publish(VOICE_RECORDING,1,"一");CHECK(quick_ui_voice_inline(&g_ui));wire_equals(0,"");
    publish(VOICE_RECORDING,2,"一个字逐步出现");wire_equals(0,"");
    publish(VOICE_RECORDING,3,"识别全文改写");CHECK(!strcmp(g_ui.voice_text,"识别全文改写"));wire_equals(0,"");
    press(KEYCODE_RETURN);CHECK(g_ui.voice_state==UI_VOICE_FINALIZING);wire_equals(0,"");
    publish(VOICE_READY,4,"最终稿");CHECK(g_ui.voice_state==UI_VOICE_READY);wire_equals(0,"");
    press(KEYCODE_ESCAPE);CHECK(g_ui.page==UI_CLOSED&&!g_ui_painter.completions&&!toast[0]);wire_equals(0,"");wire_equals(1,"");
    fresh();quick_ui_close(&g_ui);
    CHECK(quick_ui_voice_shortcut(&g_ui));ui_apply_effect();
    CHECK(g_ui.page==UI_VOICE&&g_ui.voice_state==UI_VOICE_CONNECTING);
    CHECK(starts==1&&g_voice_target==11&&!last_probe&&prepare_calls==1);wire_equals(0,"");
    CHECK(!quick_ui_voice_shortcut(&g_ui));CHECK(starts==1);
    press(KEYCODE_ESCAPE);CHECK(cancels==1);wire_equals(0,"");
    /* Final -> UTF-8 cursor edit -> connection test -> same original target.
     * Replies, short writes and retryable errors cannot interleave the paste. */
    fresh();CHECK(session_write_bytes(fake_app.slots[0],"seed",4)==4);ready("中文\n第二行\t🙂");
    uint64_t generation=g_voice_original_out;CHECK(generation==1);
    press(KEYCODE_HOME);CHECK(!quick_ui_voice_paste(&g_ui,"前",3));
    publish(VOICE_READY,3,"迟到最终");CHECK(!strcmp(g_ui.voice_text,"前中文\n第二行\t🙂"));
    health();CHECK(g_voice_original_out==generation);press(KEYCODE_RETURN);CHECK(g_voice_committing&&g_voice_commit_target==11);
    write_limit=7;voice_tick();size_t prefix=wire_size[0];
    CHECK(session_write_reply(fake_app.slots[0],"reply",5)==5);
    CHECK(session_write_bytes(fake_app.slots[0],"foreign",7)<0&&errno==EAGAIN);
    write_error=EINTR;voice_tick();CHECK(wire_size[0]==prefix&&g_voice_committing);
    write_error=EAGAIN;voice_tick();CHECK(wire_size[0]==prefix&&g_voice_committing);
    write_error=0;write_limit=1;drain();CHECK(g_ui.page==UI_CLOSED&&!g_voice_target);
    CHECK(g_ui_painter.completions==1&&!toast[0]);voice_tick();CHECK(g_ui_painter.completions==1);
    wire_equals(0,"seed\033[200~前中文 第二行 🙂\033[201~reply");wire_equals(1,"");

    /* Testing connectivity must not refresh a stale input-generation guard. */
    fresh();ready("不能放到变化后的输入行");generation=g_voice_original_out;
    CHECK(session_write_bytes(fake_app.slots[0],"changed",7)==7);health();CHECK(g_voice_original_out==generation);
    press(KEYCODE_RETURN);CHECK(!g_voice_committing&&g_ui.voice_state==UI_VOICE_ERROR);wire_equals(0,"changed");CHECK(!strcmp(clipboard,"不能放到变化后的输入行"));
    publish(VOICE_READY,100,"旧任务迟到");CHECK(g_ui.voice_state==UI_VOICE_ERROR&&!strcmp(g_ui.voice_text,clipboard));

    /* Failed and canceled health checks leave a reviewed draft recoverable. */
    fresh();ready("已有最终稿");expanded();click("Mac 连接");start_failure=1;click("测试连接");
    CHECK(g_ui.voice_state==UI_VOICE_READY&&g_voice_target==11);start_failure=0;click("测试连接");
    press(KEYCODE_ESCAPE);voice_tick();CHECK(!g_voice_probe&&g_voice_target==11&&g_ui.voice_state==UI_VOICE_READY);
    press(KEYCODE_RETURN);CHECK(g_voice_committing);drain();wire_equals(0,"\033[200~已有最终稿\033[201~");

    /* A tab switch never retargets the captured composition. */
    fresh();ready("原标签草稿");fake_app.active=1;ui_sync();voice_tick();
    CHECK(!g_voice_target&&g_ui.voice_state==UI_VOICE_ERROR&&g_ui.target==11);publish(VOICE_READY,100,"迟到");
    copy_draft();CHECK(!strcmp(clipboard,"原标签草稿"));wire_equals(0,"");wire_equals(1,"");

    /* Shell exit between Enter and effect handling must unlock INSERTING. */
    fresh();ready("退出后也能复制");key_only(KEYCODE_RETURN);CHECK(g_ui.voice_state==UI_VOICE_INSERTING);
    session_mark_exited(fake_app.slots[0],0);ui_apply_effect();
    CHECK(!g_voice_committing&&g_ui.voice_state==UI_VOICE_ERROR);copy_draft();CHECK(!strcmp(clipboard,"退出后也能复制"));
    press(KEYCODE_ESCAPE);CHECK(g_ui.page==UI_CLOSED);wire_equals(0,"");
    fresh();session_mark_exited(fake_app.slots[0],0);click("开始录音");CHECK(!starts&&g_ui.voice_state==UI_VOICE_ERROR);

    /* Removing a tab during delivery retains the immutable recovery draft. */
    fresh();ready("写入中的原稿");press(KEYCODE_RETURN);write_limit=7;voice_tick();
    session_destroy(fake_app.slots[0]);fake_app.slots[0]=NULL;fake_app.active=1;ui_sync();
    CHECK(g_ui.voice_state==UI_VOICE_INSERTING);voice_tick();CHECK(!g_voice_committing&&g_ui.voice_state==UI_VOICE_ERROR);
    CHECK(!strcmp(clipboard,"写入中的原稿"));clipboard[0]=0;copy_draft();CHECK(!strcmp(clipboard,"写入中的原稿"));wire_equals(1,"");

    /* A failed or timed-out short write completes only the split codepoint
     * and paste terminator, ahead of queued replies. Never replay the draft. */
    for(int failure=0;failure<3;failure++){
        fresh();ready("中文🙂剩余文字");press(KEYCODE_RETURN);write_limit=7;voice_tick();CHECK(wire_size[0]==7);
        CHECK(session_write_reply(fake_app.slots[0],"reply",5)==5);
        clipboard_failure=failure==2;
        write_error=failure==1?EAGAIN:EIO;if(failure==1)g_voice_commit_started.tv_sec-=11;
        voice_tick();CHECK(!g_voice_committing&&g_ui.voice_state==UI_VOICE_ERROR&&!g_ui_painter.completions);
        CHECK(!strcmp(g_ui.voice_text,"中文🙂剩余文字"));
        if(clipboard_failure){CHECK(!clipboard[0]&&strstr(g_ui.status,"复制失败"));clipboard_failure=0;copy_draft();}
        CHECK(!strcmp(clipboard,"中文🙂剩余文字"));write_error=0;write_limit=1;drain();
        wire_equals(0,"\033[200~中\033[201~reply");CHECK(session_input_generation(fake_app.slots[0])==1);
    }

    /* Acquiring ownership never splices into an older automatic response. */
    fresh();ready("可以重试");write_limit=1;CHECK(session_write_reply(fake_app.slots[0],"old",3)==3);
    write_error=EAGAIN;press(KEYCODE_RETURN);CHECK(!g_voice_committing&&g_ui.voice_state==UI_VOICE_READY);
    CHECK(!strcmp(g_ui.voice_text,"可以重试"));write_error=0;write_limit=100;
    CHECK(session_flush_replies(fake_app.slots[0])==1);press(KEYCODE_RETURN);CHECK(g_voice_committing);drain();
    wire_equals(0,"old\033[200~可以重试\033[201~");

    /* A settings-only health check owns no draft and cancels on leaving it. */
    fresh();quick_ui_open(&g_ui,UI_CONNECTION);click("测试连接");CHECK(g_voice_probe&&!g_voice_target&&last_probe&&!prepare_calls);
    press(KEYCODE_ESCAPE);voice_tick();CHECK(!g_voice_probe&&cancels==1);wire_equals(0,"");
    for(int i=0;i<2;i++)session_destroy(fake_app.slots[i]);
    printf("voice UI integration: %d checks passed (production coordinator, no audio/network)\n",checks);return 0;
}
'''


def main():
    source = (ROOT / "src/main.c").read_text()
    production = section(source, "static const char *voice_status(int state) {", "static uint64_t voice_shortcut_now(void){")
    expected = ["voice_status", "voice_error_status", "voice_screen_awake_sync", "voice_write", "voice_tick", "ui_apply_effect"]
    found = re.findall(r"^static [^\n{;]*?\b(\w+)\([^\n]*?\)\s*\{", production, re.M)
    assert found == expected, f"production voice functions changed: {found}"
    assert "UI_EFFECT_VOICE_INSERT" in production and "voice_commit_abort_tail" in production
    globals_ = section(source, "static voice_client_t *g_voice;", "static void ui_sync(void);")
    assert "static session_id_t g_voice_probe_target;" in globals_
    client = (ROOT / "src/voice_client.c").read_text()
    validator_start = re.search(r"int voice_text_valid\([^)]*\)\s*\{", client)
    assert validator_start, "missing text validator"
    validator = section(client, validator_start.group(0), "static unsigned psk(")
    assert validator.count("voice_text_valid(") == 1
    flags = shlex.split(os.environ.get("TEST_SANITIZERS", "-fsanitize=address,undefined -fno-omit-frame-pointer"))
    icu = shlex.split(os.environ.get("TEST_ICU_CFLAGS", ""))
    if not icu and Path("/opt/homebrew/opt/icu4c/include").is_dir():
        icu = ["-I/opt/homebrew/opt/icu4c/include"]
    with tempfile.TemporaryDirectory(prefix="term50-voice-ui-") as folder:
        cfile, binary = Path(folder) / "coordinator.c", Path(folder) / "coordinator"
        cfile.write_text(PRELUDE + globals_ + validator + production + TESTS)
        command = shlex.split(os.environ.get("TEST_CC", "cc")) + ["-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror=implicit-function-declaration"] + flags + icu
        command += ["-Itests/compat", "-Isrc", str(cfile), "src/quick_ui.c", "src/ui_settings.c", "src/session.c", "src/voice_commit.c", "src/bitmap.c", "-o", str(binary)]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
