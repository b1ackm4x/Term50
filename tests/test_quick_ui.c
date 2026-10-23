#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "quick_ui.h"
#include "bitmap.h"
#include <sys/keycodes.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static int touch(quick_ui_t *u,event_type_t t,int x,int y){event_t e={.type=t};e.as.touch.x=x;e.as.touch.y=y;return quick_ui_event(u,&e);}
static int key(quick_ui_t *u,int k,int down,int repeat){event_t e={.type=TERM_EVENT_KEY};e.as.key=(key_event_t){.sym=k,.pressed=down,.repeat=repeat};return quick_ui_event(u,&e);}
static void tapkey(quick_ui_t *u,int k){CHECK(key(u,k,1,0));CHECK(key(u,k,0,0));}
static void click(quick_ui_t *u,int i){CHECK(i>=0&&i<u->item_count);rect_t r=u->items[i].rect;CHECK(touch(u,TERM_EVENT_TOUCH_DOWN,r.x+r.w/2,r.y+r.h/2));CHECK(touch(u,TERM_EVENT_TOUCH_UP,r.x+r.w/2,r.y+r.h/2));}
static int label(quick_ui_t *u,const char *name){for(int i=0;i<u->item_count;i++)if(!strcmp(u->items[i].label,name))return i;return -1;}
static void clicklabel(quick_ui_t *u,const char *name){click(u,label(u,name));}
static void openvoice(quick_ui_t *u){quick_ui_open(u,UI_VOICE);if(quick_ui_voice_inline(u))clicklabel(u,"展开");}
static void openball(quick_ui_t *u){rect_t r=u->ball;CHECK(touch(u,TERM_EVENT_TOUCH_DOWN,r.x+r.w/2,r.y+r.h/2));CHECK(touch(u,TERM_EVENT_TOUCH_UP,r.x+r.w/2,r.y+r.h/2));CHECK(u->page==UI_QUICK);}
int main(void){
    ui_settings_t settings,out;ui_settings_defaults(&settings);
    CHECK(settings.opacity==60&&settings.companion_port==45850&&settings.default_profile==-1);
    CHECK(ui_settings_valid(&settings));
    char dir[]="/tmp/term50-ui-XXXXXX";CHECK(mkdtemp(dir));
    CHECK(!ui_settings_load(dir,&out));CHECK(!memcmp(&settings,&out,sizeof(out)));
    strcpy(settings.companion_host,"192.0.2.10");settings.companion_enabled=1;
    memset(settings.companion_token,'a',64);settings.companion_token[64]=0;settings.default_profile=TERM_PROFILE_CLAUDE;
    CHECK(!ui_settings_save(dir,&settings));CHECK(!ui_settings_load(dir,&out));CHECK(!memcmp(&settings,&out,sizeof(out)));
    char path[512];snprintf(path,sizeof(path),"%s/ui-settings.conf",dir);struct stat st;CHECK(!stat(path,&st));CHECK((st.st_mode&0777)==0600);
    ui_settings_t bad=settings;bad.companion_port=65536;CHECK(ui_settings_save(dir,&bad)<0);CHECK(!ui_settings_load(dir,&out));CHECK(out.companion_port==45850);
    bad=settings;strcpy(bad.companion_host,"hostname.invalid");CHECK(!ui_settings_valid(&bad));
    bad=settings;strcpy(bad.companion_host,"2001:db8::123");CHECK(ui_settings_valid(&bad));
    bad=settings;strcpy(bad.companion_token,"bad\nvalue");CHECK(!ui_settings_valid(&bad));
    bad=settings;bad.companion_token[0]=0;CHECK(!ui_settings_valid(&bad));bad.companion_enabled=0;CHECK(ui_settings_valid(&bad));
    bad=settings;bad.companion_token[63]=0;CHECK(!ui_settings_valid(&bad));bad.companion_enabled=0;CHECK(!ui_settings_valid(&bad));
    bad=settings;bad.companion_token[0]='G';CHECK(!ui_settings_valid(&bad));bad.companion_token[0]='F';CHECK(ui_settings_valid(&bad));
    FILE *f=fopen(path,"a");CHECK(f);CHECK(fputs("companion_port=22\n",f)>=0);CHECK(!fclose(f));
    CHECK(ui_settings_load(dir,&out)<0);CHECK(out.companion_enabled==0&&out.default_profile==-1);
    CHECK(!unlink(path));CHECK(!symlink("/dev/null",path));CHECK(ui_settings_load(dir,&out)<0);CHECK(!unlink(path));
    CHECK(!ui_settings_save(dir,&settings));CHECK(!chmod(path,0644));CHECK(ui_settings_load(dir,&out)<0);CHECK(!chmod(path,0600));
    quick_ui_t u;ui_settings_defaults(&settings);quick_ui_init(&u,&settings);quick_ui_geometry(&u,720,720,0);
    ui_tab_t tabs[]={{11,TERM_INPUT_NORMAL,TERM_PROFILE_CLAUDE},{25,TERM_INPUT_INSERT,TERM_PROFILE_TTY}};
    quick_ui_sync(&u,tabs,2,11);CHECK(u.ball.w==80&&u.ball.x>600);CHECK(u.profile==TERM_PROFILE_CLAUDE);
    CHECK(!key(&u,'a',1,0));CHECK(!touch(&u,TERM_EVENT_TOUCH_DOWN,100,400));CHECK(!touch(&u,TERM_EVENT_TOUCH_UP,100,400));
    rect_t ball=u.ball;CHECK(touch(&u,TERM_EVENT_TOUCH_DOWN,ball.x+20,ball.y+20));CHECK(touch(&u,TERM_EVENT_TOUCH_MOVE,200,460));CHECK(u.ball.x==180&&u.ball.y==440);CHECK(u.page==UI_CLOSED);
    CHECK(touch(&u,TERM_EVENT_TOUCH_UP,200,460));ui_effect_t e;CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_SAVE);CHECK(u.draft.side==0);CHECK(u.saved.side==1);quick_ui_save_result(&u,1);CHECK(u.ball.x==14&&u.page==UI_CLOSED);
    openball(&u);CHECK(u.items[2].selected);CHECK(!u.items[1].selected&&!u.items[3].selected);
    CHECK(key(&u,'z',1,0));click(&u,3);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_MODE_INSERT&&e.target==11);CHECK(u.page==UI_CLOSED);
    CHECK(key(&u,'z',1,1));CHECK(key(&u,'z',0,0));CHECK(!key(&u,'z',1,0));
    openball(&u);CHECK(key(&u,KEYCODE_ESCAPE,1,0));CHECK(u.page==UI_CLOSED);CHECK(key(&u,KEYCODE_ESCAPE,1,1));CHECK(key(&u,KEYCODE_ESCAPE,0,0));
    openball(&u);quick_ui_sync(&u,tabs,2,25);CHECK(u.target==11);clicklabel(&u,"中断 Ctrl-C");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_INTERRUPT&&e.target==11);
    openball(&u);quick_ui_sync(&u,tabs,1,11);CHECK(u.page==UI_CLOSED);CHECK(!quick_ui_take_effect(&u,&e));
    quick_ui_open(&u,UI_CONNECTION);clicklabel(&u,"对端监听端口");CHECK(u.page==UI_EDITOR);
    clicklabel(&u,"清空");CHECK(!quick_ui_editor_paste(&u,"65536",5));tapkey(&u,KEYCODE_RETURN);CHECK(u.page==UI_EDITOR&&u.draft.companion_port==45850);
    clicklabel(&u,"清空");CHECK(!quick_ui_editor_paste(&u,"45851",5));tapkey(&u,KEYCODE_RETURN);CHECK(u.page==UI_CONNECTION&&u.draft.companion_port==45851);
    CHECK(u.saved.companion_port==45850);clicklabel(&u,"保存");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_SAVE);
    quick_ui_save_result(&u,0);CHECK(u.saved.companion_port==45850&&u.draft.companion_port==45851);
    clicklabel(&u,"保存");CHECK(quick_ui_take_effect(&u,&e));CHECK(!ui_settings_save(dir,&u.draft));quick_ui_save_result(&u,1);CHECK(u.saved.companion_port==45851);
    clicklabel(&u,"配对密钥");CHECK(!quick_ui_editor_paste(&u,"temporary-token",15));
    for(int i=0;i<u.item_count;i++){CHECK(!strstr(u.items[i].label,"temporary-token"));CHECK(!strstr(u.items[i].detail,"temporary-token"));}
    CHECK(quick_ui_editor_paste(&u,"bad\n",4)<0);tapkey(&u,KEYCODE_ESCAPE);CHECK(!u.edit[0]&&!u.draft.companion_token[0]);
    clicklabel(&u,"对端监听端口");clicklabel(&u,"清空");tapkey(&u,KEYCODE_LEFT_ALT);tapkey(&u,'w');CHECK(!strcmp(u.edit,"1"));
    tapkey(&u,KEYCODE_LEFT_ALT);tapkey(&u,'s');CHECK(!strcmp(u.edit,"14"));
    u.dirty=0;tapkey(&u,KEYCODE_LEFT);CHECK(u.cursor==1&&u.dirty);CHECK(strstr(u.items[1].detail,"1|4"));
    u.dirty=0;tapkey(&u,KEYCODE_RIGHT);CHECK(u.cursor==2&&u.dirty);CHECK(strstr(u.items[1].detail,"14|"));
    tapkey(&u,KEYCODE_ESCAPE);clicklabel(&u,"测试连接");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_TEST);CHECK(strstr(u.status,"正在测试"));
    quick_ui_close(&u);ball=u.ball;touch(&u,TERM_EVENT_TOUCH_DOWN,ball.x+10,ball.y+10);quick_ui_geometry(&u,720,720,220);CHECK(touch(&u,TERM_EVENT_TOUCH_UP,400,400));CHECK(u.page==UI_CLOSED);
    CHECK(u.ball.x>=0&&u.ball.y>=0&&u.ball.x+u.ball.w<=720&&u.ball.y+u.ball.h<=500);
    quick_ui_open(&u,UI_APPEARANCE);for(int i=0;i<15;i++){clicklabel(&u,"静止透明度");CHECK(u.draft.opacity>=30&&u.draft.opacity<=85);}
    quick_ui_close(&u);quick_ui_geometry(&u,720,720,0);openball(&u);CHECK(key(&u,KEYCODE_LEFT_ALT,1,0));CHECK(key(&u,KEYCODE_LEFT_ALT,0,0));tapkey(&u,0xf0d3);CHECK(u.page==UI_CLOSED);
    /* Voice is a composition bound to the initiating tab, never a live PTY
     * edit. Updates replace snapshots and final text remains editable. */
    quick_ui_sync(&u,tabs,2,11);openball(&u);clicklabel(&u,"语音输入");CHECK(u.page==UI_VOICE&&u.target==11&&u.voice_state==UI_VOICE_IDLE);CHECK(quick_ui_voice_inline(&u));clicklabel(&u,"展开");CHECK(!quick_ui_voice_inline(&u));
    for(int i=0;i<u.item_count;i++){rect_t r=u.items[i].rect;CHECK(r.y>=0&&r.y+r.h<=720);}
    CHECK(u.voice_rect.y+u.voice_rect.h<=720);
    clicklabel(&u,"Mac 连接");CHECK(u.page==UI_CONNECTION&&u.connection_parent==UI_VOICE);
    CHECK(strstr(u.items[label(&u,"配对密钥")].detail,"64"));tapkey(&u,KEYCODE_ESCAPE);CHECK(u.page==UI_VOICE&&u.target==11);
    clicklabel(&u,"开始录音");CHECK(u.voice_state==UI_VOICE_CONNECTING);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_START&&e.target==11);
    tapkey(&u,KEYCODE_RETURN);clicklabel(&u,"插入终端");CHECK(!quick_ui_take_effect(&u,&e));CHECK(quick_ui_voice_paste(&u,"x",1)<0);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"中文",1,"正在聆听");CHECK(!strcmp(u.voice_text,"中文")&&u.voice_cursor==6);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"旧文本",0,NULL);CHECK(!strcmp(u.voice_text,"中文")&&u.voice_revision==1);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"中文输入",2,NULL);CHECK(!strcmp(u.voice_text,"中文输入"));
    quick_ui_sync(&u,tabs,2,25);CHECK(u.target==11);
    tapkey(&u,KEYCODE_RETURN);CHECK(u.voice_state==UI_VOICE_FINALIZING);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_STOP&&e.target==11);
    tapkey(&u,KEYCODE_RETURN);CHECK(!quick_ui_take_effect(&u,&e));
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"中文输入。",3,NULL);CHECK(u.voice_state==UI_VOICE_FINALIZING&&!strcmp(u.voice_text,"中文输入。"));
    quick_ui_voice_update(&u,UI_VOICE_READY,"你🙂好",4,"可编辑");CHECK(u.voice_state==UI_VOICE_READY&&u.voice_cursor==10);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"迟到",5,NULL);CHECK(u.voice_state==UI_VOICE_READY&&!strcmp(u.voice_text,"你🙂好")&&u.voice_revision==4);
    /* Reviewing with the cursor already fixes the final text, even before
     * its first edit. A duplicate or late final may not move that cursor. */
    tapkey(&u,KEYCODE_HOME);quick_ui_voice_update(&u,UI_VOICE_READY,"迟到的最终内容",6,"迟到的状态");
    CHECK(u.voice_cursor==0&&!strcmp(u.voice_text,"你🙂好")&&u.voice_revision==4&&!strcmp(u.status,"可编辑"));tapkey(&u,KEYCODE_END);
    tapkey(&u,KEYCODE_LEFT);CHECK(u.voice_cursor==7);tapkey(&u,KEYCODE_BACKSPACE);CHECK(!strcmp(u.voice_text,"你好")&&u.voice_cursor==3);
    tapkey(&u,KEYCODE_DELETE);CHECK(!strcmp(u.voice_text,"你")&&u.voice_cursor==3);tapkey(&u,KEYCODE_SPACE);CHECK(!strcmp(u.voice_text,"你 "));
    event_t uni={.type=TERM_EVENT_KEY};uni.as.key=(key_event_t){.sym=0x4e2d,.unicode=0x4e2d,.pressed=1};CHECK(quick_ui_event(&u,&uni));uni.as.key.pressed=0;CHECK(quick_ui_event(&u,&uni));CHECK(!strcmp(u.voice_text,"你 中"));
    tapkey(&u,KEYCODE_HOME);CHECK(u.voice_cursor==0);tapkey(&u,KEYCODE_DELETE);CHECK(!strcmp(u.voice_text," 中"));
    tapkey(&u,KEYCODE_END);CHECK(u.voice_cursor==strlen(u.voice_text));CHECK(!quick_ui_voice_paste(&u,"终端",6));CHECK(!strcmp(u.voice_text," 中终端"));
    CHECK(quick_ui_voice_paste(&u,"\xc0\xaf",2)<0);CHECK(quick_ui_voice_paste(&u,"\xed\xa0\x80",3)<0);CHECK(quick_ui_voice_paste(&u,"\x1b[2J",4)<0);CHECK(quick_ui_voice_paste(&u,"x\0y",3)<0);
    quick_ui_voice_update(&u,UI_VOICE_READY,"服务器修改",6,NULL);CHECK(!strcmp(u.voice_text," 中终端")&&u.voice_edited);
    clicklabel(&u,"粘贴到草稿");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_PASTE&&e.target==11);
    tapkey(&u,KEYCODE_RETURN);CHECK(u.voice_state==UI_VOICE_INSERTING);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_INSERT&&e.target==11);CHECK(!strcmp(u.voice_text," 中终端"));
    tapkey(&u,KEYCODE_RETURN);CHECK(!quick_ui_take_effect(&u,&e));tapkey(&u,KEYCODE_ESCAPE);CHECK(u.page==UI_VOICE&&u.voice_state==UI_VOICE_INSERTING);CHECK(!strcmp(u.status,"正在插入，请稍候"));
    clicklabel(&u,"取消");CHECK(u.page==UI_VOICE&&!quick_ui_take_effect(&u,&e));click(&u,0);CHECK(u.page==UI_VOICE&&!quick_ui_take_effect(&u,&e));
    quick_ui_open(&u,UI_SETTINGS);CHECK(u.page==UI_VOICE&&u.target==11&&!strcmp(u.voice_text," 中终端"));
    openvoice(&u);CHECK(u.voice_state==UI_VOICE_INSERTING&&!strcmp(u.voice_text," 中终端"));
    tapkey(&u,KEYCODE_LEFT_ALT);tapkey(&u,0xf0d3);CHECK(u.page==UI_VOICE&&!quick_ui_take_effect(&u,&e));
    /* The coordinator may close the UI after the immutable commit drains. */
    quick_ui_close(&u);CHECK(u.page==UI_CLOSED&&!quick_ui_take_effect(&u,&e));
    /* A late recognizer cannot resurrect a closed panel or target another tab. */
    quick_ui_voice_update(&u,UI_VOICE_READY,"过期结果",7,NULL);CHECK(u.page==UI_CLOSED);
    openvoice(&u);CHECK(u.target==25&&u.voice_state==UI_VOICE_IDLE&&!u.voice_text[0]);clicklabel(&u,"开始录音");CHECK(quick_ui_take_effect(&u,&e));
    quick_ui_sync(&u,tabs,1,11);CHECK(u.page==UI_VOICE&&u.voice_state==UI_VOICE_ERROR&&u.target==25);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL&&e.target==25);
    CHECK(!u.items[label(&u,"开始录音")].action);tapkey(&u,KEYCODE_ESCAPE);CHECK(u.page==UI_CLOSED);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL);
    openvoice(&u);quick_ui_voice_update(&u,UI_VOICE_READY,"",1,NULL);tapkey(&u,KEYCODE_RETURN);CHECK(!quick_ui_take_effect(&u,&e));
    char large[UI_VOICE_TEXT_MAX+2];memset(large,'x',sizeof(large));large[sizeof(large)-1]=0;
    CHECK(quick_ui_voice_paste(&u,large,UI_VOICE_TEXT_MAX+1)<0);CHECK(!quick_ui_voice_paste(&u,large,UI_VOICE_TEXT_MAX));CHECK(strlen(u.voice_text)==UI_VOICE_TEXT_MAX);CHECK(quick_ui_voice_paste(&u,"中",3)<0);
    tapkey(&u,KEYCODE_ESCAPE);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL);
    openvoice(&u);quick_ui_voice_update(&u,UI_VOICE_RECORDING,"保留这段草稿",1,NULL);quick_ui_voice_update(&u,UI_VOICE_ERROR,NULL,1,"连接中断");
    CHECK(label(&u,"复制草稿")>=0&&label(&u,"插入终端")<0);tapkey(&u,KEYCODE_RETURN);CHECK(!quick_ui_take_effect(&u,&e));
    clicklabel(&u,"复制草稿");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_COPY&&e.target==11);CHECK(u.page==UI_VOICE&&!strcmp(u.voice_text,"保留这段草稿"));
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"迟到部分",2,NULL);quick_ui_voice_update(&u,UI_VOICE_READY,"迟到最终",3,NULL);
    CHECK(u.voice_state==UI_VOICE_ERROR&&u.voice_revision==1&&!strcmp(u.voice_text,"保留这段草稿"));
    tapkey(&u,KEYCODE_ESCAPE);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL);
    openvoice(&u);quick_ui_voice_update(&u,UI_VOICE_RECORDING,"\xf4\x90\x80\x80",1,NULL);CHECK(u.voice_state==UI_VOICE_ERROR&&!u.voice_text[0]);CHECK(label(&u,"复制草稿")<0);
    quick_ui_close(&u);
    /* Neither a newly enabled insert button nor a changed start/stop button
     * may activate on UP from a gesture that began in an earlier state. */
    openvoice(&u);quick_ui_voice_update(&u,UI_VOICE_RECORDING,"还在识别",1,NULL);
    rect_t insert=u.items[label(&u,"插入终端")].rect;
    CHECK(touch(&u,TERM_EVENT_TOUCH_DOWN,insert.x+10,insert.y+10));
    quick_ui_voice_update(&u,UI_VOICE_READY,"可以审阅了",2,NULL);
    CHECK(touch(&u,TERM_EVENT_TOUCH_UP,insert.x+10,insert.y+10));CHECK(u.voice_state==UI_VOICE_READY&&!quick_ui_take_effect(&u,&e));
    clicklabel(&u,"重新录音");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_START);CHECK(u.voice_state==UI_VOICE_CONNECTING&&!u.voice_text[0]);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"不能丢弃",1,NULL);rect_t stop=u.items[label(&u,"结束录音")].rect;
    CHECK(touch(&u,TERM_EVENT_TOUCH_DOWN,stop.x+10,stop.y+10));quick_ui_voice_update(&u,UI_VOICE_READY,"不能丢弃",2,NULL);
    CHECK(touch(&u,TERM_EVENT_TOUCH_UP,stop.x+10,stop.y+10));CHECK(u.voice_state==UI_VOICE_READY&&!quick_ui_take_effect(&u,&e)&&!strcmp(u.voice_text,"不能丢弃"));
    /* A connection visit leaves the draft bound to its original tab. The
     * coordinator can still invalidate it while the address editor is open. */
    clicklabel(&u,"Mac 连接");CHECK(quick_ui_voice_context(&u));clicklabel(&u,"对端 IP 地址");CHECK(quick_ui_voice_context(&u));
    quick_ui_voice_update(&u,UI_VOICE_READY,"迟到",3,NULL);CHECK(!strcmp(u.voice_text,"不能丢弃"));
    tapkey(&u,KEYCODE_ESCAPE);tapkey(&u,KEYCODE_ESCAPE);CHECK(u.page==UI_VOICE&&u.voice_state==UI_VOICE_READY&&u.target==11);
    tapkey(&u,KEYCODE_RETURN);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_INSERT);CHECK(u.voice_state==UI_VOICE_INSERTING);
    /* The PTY can exit between this effect and coordinator handling. This is
     * the same helper used by main's rejected-start/rejected-insert path. */
    quick_ui_voice_target_lost(&u);CHECK(u.voice_state==UI_VOICE_ERROR&&u.page==UI_VOICE);CHECK(label(&u,"复制草稿")>=0);
    quick_ui_sync(&u,tabs+1,1,25);CHECK(u.target==11&&u.page==UI_VOICE&&!strcmp(u.voice_text,"不能丢弃"));
    clicklabel(&u,"复制草稿");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_COPY&&e.target==11);
    tapkey(&u,KEYCODE_ESCAPE);CHECK(u.page==UI_CLOSED);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL);
    /* Deleting a target preserves both a ready draft and an in-flight commit.
     * Only the coordinator can finish INSERTING; no key can unlock it. */
    quick_ui_sync(&u,tabs,2,11);openvoice(&u);quick_ui_voice_update(&u,UI_VOICE_READY,"完整草稿",1,NULL);
    quick_ui_sync(&u,tabs+1,1,25);CHECK(u.page==UI_VOICE&&u.voice_state==UI_VOICE_ERROR&&!strcmp(u.voice_text,"完整草稿"));
    CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL);clicklabel(&u,"复制草稿");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_COPY);
    quick_ui_close(&u);quick_ui_sync(&u,tabs,2,11);openvoice(&u);quick_ui_voice_update(&u,UI_VOICE_READY,"正在写入",1,NULL);
    tapkey(&u,KEYCODE_RETURN);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_INSERT);
    quick_ui_sync(&u,tabs+1,1,25);CHECK(u.page==UI_VOICE&&u.voice_state==UI_VOICE_INSERTING&&!quick_ui_take_effect(&u,&e));
    quick_ui_voice_update(&u,UI_VOICE_READY,"不能替换正在写入的内容",2,NULL);tapkey(&u,KEYCODE_ESCAPE);
    CHECK(u.voice_state==UI_VOICE_INSERTING&&!strcmp(u.voice_text,"正在写入")&&!quick_ui_take_effect(&u,&e));
    quick_ui_voice_target_lost(&u);clicklabel(&u,"复制草稿");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_COPY);quick_ui_close(&u);
    quick_ui_sync(&u,tabs,2,11);openvoice(&u);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"相同预览",1,"正在聆听");
    u.dirty=0;
    for(unsigned revision=2;revision<=1001;revision++)
        quick_ui_voice_update(&u,UI_VOICE_RECORDING,"相同预览",revision,"正在聆听");
    CHECK(!u.dirty&&u.voice_revision==1001);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"迟到的不同文字",1000,NULL);
    CHECK(!u.dirty&&!strcmp(u.voice_text,"相同预览"));
    quick_ui_voice_update(&u,UI_VOICE_READY,"相同预览",1002,"可以编辑");
    CHECK(u.dirty&&u.voice_state==UI_VOICE_READY&&u.voice_revision==1002);
    quick_ui_close(&u);
    /* Default dictation owns input while leaving only a compact pill visible.
     * Expanding is optional and must not reset the original draft or target. */
    quick_ui_sync(&u,tabs,2,11);quick_ui_open(&u,UI_VOICE);
    CHECK(quick_ui_voice_inline(&u)&&!u.voice_expanded&&u.voice_rect.w==0);
    CHECK(label(&u,"Mac 连接")<0&&u.item_count==3);
    CHECK(u.voice_capsule.w==440&&u.voice_capsule.h==64&&u.voice_capsule.x==140&&u.voice_capsule.y==632);
    for(int i=0;i<u.item_count;i++){rect_t r=u.items[i].rect;CHECK(r.x>=u.voice_capsule.x&&r.y==u.voice_capsule.y&&r.x+r.w<=u.voice_capsule.x+u.voice_capsule.w&&r.h>=48);}
    CHECK(touch(&u,TERM_EVENT_TOUCH_DOWN,100,100));CHECK(touch(&u,TERM_EVENT_TOUCH_UP,100,100));CHECK(!quick_ui_take_effect(&u,&e));
    clicklabel(&u,"开始录音");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_START&&quick_ui_voice_inline(&u));
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"文字正在出现",1,"正在聆听");
    tapkey(&u,'x');CHECK(!strcmp(u.voice_text,"文字正在出现")&&!u.voice_edited);
    rect_t compact_stop=u.items[label(&u,"停止")].rect;
    CHECK(touch(&u,TERM_EVENT_TOUCH_DOWN,compact_stop.x+10,compact_stop.y+10));
    quick_ui_voice_update(&u,UI_VOICE_READY,"整理后的文字",2,"可以编辑");
    CHECK(touch(&u,TERM_EVENT_TOUCH_UP,compact_stop.x+10,compact_stop.y+10));CHECK(!quick_ui_take_effect(&u,&e)&&u.voice_state==UI_VOICE_READY);
    CHECK(label(&u,"回车确认")>=0&&label(&u,"修改")>=0&&quick_ui_voice_inline(&u));
    tapkey(&u,KEYCODE_END);tapkey(&u,KEYCODE_BACKSPACE);CHECK(!strcmp(u.voice_text,"整理后的文")&&u.voice_edited);
    quick_ui_voice_update(&u,UI_VOICE_READY,"迟到结果",3,NULL);CHECK(!strcmp(u.voice_text,"整理后的文")&&u.voice_revision==2);
    size_t preserved_cursor=u.voice_cursor;clicklabel(&u,"修改");CHECK(!quick_ui_voice_inline(&u)&&u.voice_expanded&&u.target==11&&u.voice_cursor==preserved_cursor&&!strcmp(u.status,"可以编辑"));
    click(&u,0);CHECK(quick_ui_voice_inline(&u)&&!u.voice_expanded&&u.target==11&&!strcmp(u.voice_text,"整理后的文")&&!strcmp(u.status,"可以编辑"));
    /* Avoidance moves every hit region and cancels an already begun tap. */
    rect_t confirm=u.items[label(&u,"回车确认")].rect;
    CHECK(touch(&u,TERM_EVENT_TOUCH_DOWN,confirm.x+10,confirm.y+10));
    quick_ui_voice_capsule_position(&u,100);CHECK(u.voice_capsule.y==100);
    for(int i=0;i<u.item_count;i++)CHECK(u.items[i].rect.y==100);
    CHECK(touch(&u,TERM_EVENT_TOUCH_UP,confirm.x+10,110));CHECK(!quick_ui_take_effect(&u,&e));
    clicklabel(&u,"回车确认");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_INSERT&&e.target==11&&u.voice_state==UI_VOICE_INSERTING);
    tapkey(&u,KEYCODE_ESCAPE);CHECK(quick_ui_voice_inline(&u)&&!quick_ui_take_effect(&u,&e));
    quick_ui_close(&u);
    CHECK(quick_ui_voice_shortcut(&u)&&quick_ui_voice_inline(&u)&&u.voice_state==UI_VOICE_CONNECTING);
    CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_START&&e.target==11);
    quick_ui_voice_update(&u,UI_VOICE_RECORDING,"已有草稿",1,NULL);tapkey(&u,KEYCODE_RETURN);
    CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_STOP&&u.voice_state==UI_VOICE_FINALIZING&&quick_ui_voice_inline(&u));
    quick_ui_voice_update(&u,UI_VOICE_ERROR,NULL,1,"无线连接已断开");CHECK(label(&u,"复制")>=0&&label(&u,"重试")>=0&&u.item_count==4);
    clicklabel(&u,"详情");CHECK(u.voice_expanded&&!strcmp(u.status,"无线连接已断开")&&!strcmp(u.voice_text,"已有草稿"));
    click(&u,0);clicklabel(&u,"复制");CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_COPY);
    quick_ui_sync(&u,tabs+1,1,25);CHECK(u.voice_state==UI_VOICE_ERROR&&quick_ui_voice_inline(&u)&&u.target==11);
    CHECK(!u.items[label(&u,"重试")].action);clicklabel(&u,"详情");CHECK(u.voice_expanded&&label(&u,"复制草稿")>=0);
    tapkey(&u,KEYCODE_ESCAPE);CHECK(u.page==UI_CLOSED&&quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL);
    quick_ui_sync(&u,tabs,2,11);quick_ui_open(&u,UI_VOICE);quick_ui_geometry(&u,720,720,220);
    CHECK(quick_ui_voice_inline(&u)&&u.voice_capsule.h>=48&&u.voice_capsule.y+u.voice_capsule.h<=u.usable_h);
    for(int i=0;i<u.item_count;i++)CHECK(u.items[i].rect.h>=48&&u.items[i].rect.y+u.items[i].rect.h<=u.usable_h);
    tapkey(&u,KEYCODE_ESCAPE);CHECK(quick_ui_take_effect(&u,&e)&&e.kind==UI_EFFECT_VOICE_CANCEL);quick_ui_geometry(&u,720,720,0);
    bitmap_t *dst=bitmap_alloc(3,3,BITMAP_FMT_RGBA8888),*src=bitmap_alloc(2,2,BITMAP_FMT_RGBA8888);CHECK(dst&&src);
    bitmap_fill_rect(dst,NULL,(rgb_t){20,40,60});bitmap_fill_rect(src,NULL,(rgb_t){220,140,80});
    for(int y=0;y<2;y++)for(int x=0;x<2;x++)src->pixels[y*src->stride+x*4+3]=128;
    bitmap_blend(dst,1,1,src);unsigned char *p=dst->pixels+dst->stride+4;CHECK(p[2]==120&&p[1]==90&&p[0]==70);CHECK(dst->pixels[2]==20);
    bitmap_blend(dst,-1,-1,src);bitmap_blend(dst,50,50,src);bitmap_free(src);bitmap_free(dst);
    CHECK(!unlink(path));CHECK(!rmdir(dir));printf("quick UI: %d checks passed\n",checks);return 0;
}
