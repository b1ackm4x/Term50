#include "quick_ui.h"
#include <sys/keycodes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef KEYCODE_BB_SYM_KEY
#define KEYCODE_BB_SYM_KEY 0xF0D3
#endif

enum { A_CLOSE=1,A_BACK,A_NORMAL,A_CLOUD,A_INSERT,A_REMOTE,A_TABS,A_PASTE,
    A_INTERRUPT,A_SETTINGS,A_CONNECTION,A_INPUT,A_APPEARANCE,A_BINDINGS,
    A_HOST,A_PORT,A_TOKEN,A_ENABLED,A_SAVE,A_TEST,A_DEFAULT,A_SIDE,A_HEIGHT,
    A_OPACITY,A_EDGE,A_TAB_NEW,A_TAB_SELECT,A_EDIT_DONE,A_EDIT_CLEAR,A_EDIT_CANCEL,A_EDIT_PASTE,
    A_VOICE_START,A_VOICE_STOP,A_VOICE_CANCEL,A_VOICE_INSERT,A_VOICE_PASTE,A_VOICE_COPY,
    A_VOICE_EXPAND,A_VOICE_COLLAPSE };
static int clamp(int x,int a,int b){return x<a?a:x>b?b:x;}
static int hit(rect_t r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
static int px(const quick_ui_t *u,int n){return n*u->scale/1000;}
static int live(const quick_ui_t *u,session_id_t id){for(unsigned i=0;i<u->tab_count;i++)if(u->tabs[i].id==id)return 1;return 0;}
static void effect(quick_ui_t *u,ui_effect_kind_t kind,int arg){u->effect=(ui_effect_t){kind,u->target,arg};}
static void block(quick_ui_t *u,int sym){if(sym>0&&sym<65536)u->blocked[sym>>3]|=1u<<(sym&7);}
static int blocked(quick_ui_t *u,int sym){return sym>0&&sym<65536&&(u->blocked[sym>>3]&(1u<<(sym&7)));}
static int voice_busy(const quick_ui_t *u){return u->voice_state==UI_VOICE_CONNECTING||u->voice_state==UI_VOICE_RECORDING||u->voice_state==UI_VOICE_FINALIZING||u->voice_state==UI_VOICE_INSERTING;}
static void voice_reset(quick_ui_t *u){memset(u->voice_text,0,sizeof(u->voice_text));u->voice_state=UI_VOICE_IDLE;u->voice_revision=0;u->voice_cursor=0;u->voice_edited=0;u->voice_scroll=0;}
static size_t utf8_prev(const char *s,size_t p){if(p){p--;while(p&&((unsigned char)s[p]&0xc0)==0x80)p--;}return p;}
static size_t utf8_next(const char *s,size_t p){if(s[p]){p++;while(((unsigned char)s[p]&0xc0)==0x80)p++;}return p;}
static int utf8_text(const char *s,size_t n){
    for(size_t p=0;p<n;){
        unsigned char c=(unsigned char)s[p++];unsigned cp;size_t more;
        if(c<0x80){if((c<32&&c!='\n'&&c!='\t')||c==127)return 0;continue;}
        if(c>=0xc2&&c<=0xdf){cp=c&31;more=1;}
        else if(c>=0xe0&&c<=0xef){cp=c&15;more=2;}
        else if(c>=0xf0&&c<=0xf4){cp=c&7;more=3;}
        else return 0;
        if(n-p<more)return 0;
        size_t count=more;
        while(more--){c=(unsigned char)s[p++];if((c&0xc0)!=0x80)return 0;cp=(cp<<6)|(c&63);}
        if((count==2&&cp<0x800)||(count==3&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||(cp>=0x80&&cp<0xa0))return 0;
    }
    return 1;
}
int quick_ui_voice_context(const quick_ui_t *u){
    return u->page==UI_VOICE||(u->connection_parent==UI_VOICE&&
        (u->page==UI_CONNECTION||(u->page==UI_EDITOR&&u->editor_parent==UI_CONNECTION)));
}
int quick_ui_voice_inline(const quick_ui_t *u){return u->page==UI_VOICE&&!u->voice_expanded;}
void quick_ui_voice_update(quick_ui_t *u,int state,const char *text,unsigned revision,const char *status){
    if(!quick_ui_voice_context(u)||state<UI_VOICE_IDLE||state>UI_VOICE_INSERTING||revision<u->voice_revision)return;
    /* A final/error snapshot ends this recognition. Only an explicit restart
     * can reopen it; cursor movement is already review of the final text. */
    if(text&&(u->voice_state==UI_VOICE_READY||u->voice_state==UI_VOICE_ERROR||u->voice_state==UI_VOICE_INSERTING))return;
    /* Final text and a local edit are immutable to late recognizer partials. */
    if((u->voice_state==UI_VOICE_READY||u->voice_state==UI_VOICE_INSERTING)&&state>=UI_VOICE_CONNECTING&&state<=UI_VOICE_FINALIZING)return;
    if(u->voice_state==UI_VOICE_FINALIZING&&(state==UI_VOICE_CONNECTING||state==UI_VOICE_RECORDING))state=UI_VOICE_FINALIZING;
    if(u->voice_state==UI_VOICE_RECORDING&&state==UI_VOICE_CONNECTING)return;
    int changed=state!=u->voice_state||(status&&strcmp(status,u->status));
    if(text&&!u->voice_edited){
        size_t n=strnlen(text,sizeof(u->voice_text));
        if(n>UI_VOICE_TEXT_MAX||!utf8_text(text,n)){quick_ui_message(u,"识别文本无效或超过长度上限");u->voice_state=UI_VOICE_ERROR;quick_ui_layout(u);return;}
        if(strcmp(u->voice_text,text)){memcpy(u->voice_text,text,n+1);u->voice_cursor=n;changed=1;}
    }
    /* Revisions order snapshots; they aren't visible content. Repainting the
     * entire phone panel for identical partials can starve audio capture. Keep
     * the ordering guard current even when no pixels need to change. */
    u->voice_revision=revision;
    if(!changed)return;
    u->voice_state=state;
    if(status)snprintf(u->status,sizeof(u->status),"%s",status);
    u->dirty=1;quick_ui_layout(u);
}
void quick_ui_voice_target_lost(quick_ui_t *u){
    if(!quick_ui_voice_context(u))return;
    u->page=UI_VOICE;memset(u->edit,0,sizeof(u->edit));u->draft=u->saved;
    quick_ui_voice_update(u,UI_VOICE_ERROR,NULL,u->voice_revision,
        u->voice_text[0]?"原标签页已关闭；可复制保留的草稿":"原标签页已关闭，请返回后选择其他标签页");
}
int quick_ui_voice_paste(quick_ui_t *u,const char *text,size_t n){
    size_t len=strlen(u->voice_text);
    if(u->page!=UI_VOICE||u->voice_state!=UI_VOICE_READY||!text||n>UI_VOICE_TEXT_MAX-len||!utf8_text(text,n))return -1;
    if(!n)return 0;
    memmove(u->voice_text+u->voice_cursor+n,u->voice_text+u->voice_cursor,len-u->voice_cursor+1);
    memcpy(u->voice_text+u->voice_cursor,text,n);u->voice_cursor+=n;u->voice_edited=1;u->dirty=1;quick_ui_layout(u);return 0;
}

void quick_ui_message(quick_ui_t *u,const char *text){snprintf(u->status,sizeof(u->status),"%s",text?text:"");u->dirty=1;}
void quick_ui_init(quick_ui_t *u,const ui_settings_t *s){memset(u,0,sizeof(*u));u->saved=u->draft=*s;u->scale=1000;u->down_item=-1;u->dirty=1;}
int quick_ui_active(const quick_ui_t *u){return u->page!=UI_CLOSED;}
void quick_ui_geometry(quick_ui_t *u,int w,int h,int keyboard_height){
    int usable=clamp(h-keyboard_height,200,h);
    if(u->w==w&&u->h==h&&u->usable_h==usable)return;
    u->w=w;u->h=h;u->usable_h=usable;u->scale=(w<usable?w:usable)*1000/720;
    if(u->scale<300)u->scale=300;
    /* Keep ownership until UP even if a resize cancels the gesture. */
    u->ball_pointer=0;u->moved=1;u->down_item=-1;u->dirty=1;quick_ui_layout(u);
}
void quick_ui_sync(quick_ui_t *u,const ui_tab_t *tabs,unsigned count,session_id_t active){
    if(count>8)count=8;
    int changed=u->active!=active||count!=u->tab_count;
    for(unsigned i=0;i<count;i++)if(i>=u->tab_count||tabs[i].id!=u->tabs[i].id||tabs[i].mode!=u->tabs[i].mode||tabs[i].profile!=u->tabs[i].profile)changed=1;
    memcpy(u->tabs,tabs,count*sizeof(*tabs));u->tab_count=count;u->active=active;
    for(unsigned i=0;i<count;i++)if(tabs[i].id==active){u->mode=tabs[i].mode;u->profile=tabs[i].profile;}
    if(u->target&&!live(u,u->target)&&u->page!=UI_CLOSED){
        if(quick_ui_voice_context(u)){
            /* An in-flight commit retains ownership until its coordinator
             * repairs any delivered prefix and reports the failure. */
            if(u->voice_state!=UI_VOICE_INSERTING&&u->voice_state!=UI_VOICE_ERROR){
                effect(u,UI_EFFECT_VOICE_CANCEL,0);quick_ui_voice_target_lost(u);
            }
        }else{quick_ui_close(u);quick_ui_message(u,"原标签页已关闭");}
    }
    if(changed){u->dirty=1;quick_ui_layout(u);}
}
void quick_ui_open(quick_ui_t *u,ui_page_t page){
    if(u->page==UI_VOICE&&u->voice_state==UI_VOICE_INSERTING){quick_ui_message(u,"正在插入，请稍候");return;}
    if(!u->active)return;
    u->target=u->active;u->page=page;u->draft=u->saved;
    if(page==UI_VOICE){voice_reset(u);u->voice_expanded=0;}
    if(page==UI_CONNECTION)u->connection_parent=UI_SETTINGS;
    u->status[0]=0;u->alt=u->shift=u->sym=0;u->dirty=1;quick_ui_layout(u);
}
void quick_ui_close(quick_ui_t *u){
    u->page=UI_CLOSED;u->alt=u->shift=u->sym=0;
    memset(u->edit,0,sizeof(u->edit));u->draft=u->saved;u->dirty=1;
}
int quick_ui_voice_shortcut(quick_ui_t *u){
    if(u->page!=UI_CLOSED||!u->active)return 0;
    quick_ui_open(u,UI_VOICE);
    u->voice_state=UI_VOICE_CONNECTING;
    effect(u,UI_EFFECT_VOICE_START,0);
    quick_ui_layout(u);return 1;
}
static void add(quick_ui_t *u,int x,int y,int w,int h,int action,int value,ui_icon_t icon,const char *label,const char *detail){
    if(u->item_count==UI_ITEM_MAX)return;
    ui_item_t *p=&u->items[u->item_count++];memset(p,0,sizeof(*p));
    p->rect=(rect_t){px(u,x),px(u,y),px(u,w),px(u,h)};p->action=action;p->value=value;p->icon=icon;
    snprintf(p->label,sizeof(p->label),"%s",label?label:"");snprintf(p->detail,sizeof(p->detail),"%s",detail?detail:"");
}
static void row(quick_ui_t *u,int y,int action,int value,const char *label,const char *detail){add(u,28,y,664,76,action,value,UI_ICON_NONE,label,detail);}
static void voice_inline_item(quick_ui_t *u,int x,int w,int action,const char *label){
    add(u,0,0,0,0,action,0,UI_ICON_NONE,label,NULL);
    u->items[u->item_count-1].rect=(rect_t){u->voice_capsule.x+x,u->voice_capsule.y,w,u->voice_capsule.h};
}
static void voice_inline_layout(quick_ui_t *u){
    int margin=px(u,24);if(margin<12)margin=12;
    int w=px(u,440),h=px(u,64);if(w<224)w=224;if(w>u->w-2*margin)w=u->w-2*margin;if(h<48)h=48;
    int y=u->usable_h-margin-h;if(y<px(u,84))y=px(u,84);
    u->voice_capsule=(rect_t){(u->w-w)/2,y,w,h};
    u->voice_rect=(rect_t){0,0,0,0};
    int edge=8,gap=4,cancel=48,button=px(u,96);if(button<64)button=64;
    int right=w-edge-cancel,left_width=right-gap-edge;
    int live_target=live(u,u->target);
    switch(u->voice_state){
    case UI_VOICE_IDLE:case UI_VOICE_READY:
        left_width-=button+gap;
        voice_inline_item(u,edge,left_width,live_target&&(u->voice_state==UI_VOICE_IDLE||u->voice_text[0])?
            (u->voice_state==UI_VOICE_IDLE?A_VOICE_START:A_VOICE_INSERT):0,
            u->voice_state==UI_VOICE_IDLE?"开始录音":"回车确认");
        voice_inline_item(u,edge+left_width+gap,button,A_VOICE_EXPAND,u->voice_state==UI_VOICE_IDLE?"展开":"修改");
        break;
    case UI_VOICE_ERROR:{
        int small=px(u,72);if(small<48)small=48;
        int copy=u->voice_text[0]!=0;
        left_width-=(small+gap)*(1+copy);
        voice_inline_item(u,edge,left_width,A_VOICE_EXPAND,"详情");
        voice_inline_item(u,edge+left_width+gap,small,live_target?A_VOICE_START:0,"重试");
        if(copy)voice_inline_item(u,edge+left_width+gap+small+gap,small,A_VOICE_COPY,"复制");
        break;}
    case UI_VOICE_RECORDING:
        left_width-=button+gap;
        voice_inline_item(u,edge,left_width,0,"聆听中");
        voice_inline_item(u,edge+left_width+gap,button,A_VOICE_STOP,"停止");
        break;
    default:
        voice_inline_item(u,edge,left_width,0,u->voice_state==UI_VOICE_CONNECTING?"连接中":
            u->voice_state==UI_VOICE_FINALIZING?"整理中":"正在插入");break;
    }
    voice_inline_item(u,right,cancel,u->voice_state==UI_VOICE_INSERTING?0:A_VOICE_CANCEL,"取消");
}
void quick_ui_voice_capsule_position(quick_ui_t *u,int y){
    if(!quick_ui_voice_inline(u))return;
    y=clamp(y,px(u,84),u->usable_h-u->voice_capsule.h);
    int dy=y-u->voice_capsule.y;if(!dy)return;
    u->voice_capsule.y=y;
    for(int i=0;i<u->item_count;i++)u->items[i].rect.y+=dy;
    /* Moving controls invalidates an in-progress tap, but keeps ownership
     * through release so it cannot become a terminal gesture. */
    u->down_item=-1;u->dirty=1;
}
void quick_ui_layout(quick_ui_t *u){
    int size=px(u,80),margin=px(u,14),height=u->usable_h;
    if(!u->ball_pointer){
        u->ball.w=u->saved.edge_handle?px(u,60):size;u->ball.h=size;
        u->ball.x=u->saved.side?u->w-margin-u->ball.w:margin;
        int top=px(u,84),travel=height-top-size-px(u,32);if(travel<0)travel=0;
        u->ball.y=top+travel*u->saved.height_permille/1000;
    }
    u->item_count=0;u->title[0]=0;
    if(u->page==UI_CLOSED)return;
    if(quick_ui_voice_inline(u)){voice_inline_layout(u);return;}
    add(u,28,22,80,60,A_BACK,0,UI_ICON_BACK,"",NULL);
    switch(u->page){
    case UI_QUICK:
        u->items[0].rect.y+=px(u,60);
        snprintf(u->title,sizeof(u->title),"快捷操作 · Tab %u",u->target);
        add(u,28,118,208,82,A_NORMAL,0,UI_ICON_TERMINAL,"",NULL);
        u->items[u->item_count-1].selected=u->profile==TERM_PROFILE_TERMINAL;
        add(u,256,118,208,82,A_CLOUD,0,UI_ICON_CLOUD,"",NULL);
        u->items[u->item_count-1].selected=u->profile==TERM_PROFILE_CLAUDE;
        add(u,484,118,208,82,A_INSERT,0,UI_ICON_KEYBOARD,"",NULL);
        u->items[u->item_count-1].selected=u->profile==TERM_PROFILE_TTY;
        add(u,28,236,320,98,A_REMOTE,0,UI_ICON_NONE,"语音输入","豆包 · 流式转写");
        add(u,372,236,320,98,A_TABS,0,UI_ICON_NONE,"标签页","切换 / 新建");
        add(u,28,346,320,98,A_PASTE,0,UI_ICON_NONE,"粘贴","不自动回车");
        add(u,372,346,320,98,A_INTERRUPT,0,UI_ICON_NONE,"中断 Ctrl-C","当前 Tab");
        row(u,470,A_SETTINGS,0,"设置","连接、输入、悬浮入口");
        for(int i=1;i<u->item_count;i++)u->items[i].rect.y+=px(u,60);
        break;
    case UI_SETTINGS:
        strcpy(u->title,"设置");
        row(u,120,A_CONNECTION,0,"Mac 语音连接","地址、端口、配对密钥");
        row(u,224,A_INPUT,0,"输入与按键","当前模式 / 默认模式");
        row(u,328,A_APPEARANCE,0,"悬浮入口","位置、透明度、外观");break;
    case UI_CONNECTION:{
        char port[48];snprintf(port,sizeof(port),"TCP %d",u->draft.companion_port);
        strcpy(u->title,"Mac 语音连接");
        row(u,108,A_ENABLED,0,"启用语音连接",u->draft.companion_enabled?"是 · 点击录音时连接":"否");
        row(u,196,A_HOST,0,"对端 IP 地址",u->draft.companion_host[0]?u->draft.companion_host:"点击填写");
        row(u,284,A_PORT,0,"对端监听端口",port);
        row(u,372,A_TOKEN,0,"配对密钥",u->draft.companion_token[0]?"已设置 · 点击替换":"粘贴 Mac 生成的 64 位十六进制密钥");
        add(u,28,472,320,76,A_TEST,0,UI_ICON_NONE,"测试连接",NULL);
        add(u,372,472,320,76,A_SAVE,0,UI_ICON_NONE,"保存",NULL);break;}
    case UI_VOICE:{
        u->items[0].action=A_VOICE_COLLAPSE;
        static const char *states[]={"准备录音","正在连接","正在聆听","正在整理","可以编辑后插入","连接或识别失败","正在插入"};
        snprintf(u->title,sizeof(u->title),"语音输入 · Tab %u",u->target);
        row(u,100,voice_busy(u)?0:A_CONNECTION,0,"Mac 连接",u->saved.companion_host[0]?u->saved.companion_host:"先配置地址与配对密钥");
        u->voice_rect=(rect_t){px(u,28),px(u,192),px(u,664),px(u,266)};
        int a=0;const char *label=states[u->voice_state];
        if(u->voice_state==UI_VOICE_IDLE||u->voice_state==UI_VOICE_ERROR){a=A_VOICE_START;label="开始录音";}
        else if(u->voice_state==UI_VOICE_RECORDING){a=A_VOICE_STOP;label="结束录音";}
        else if(u->voice_state==UI_VOICE_READY){a=A_VOICE_START;label="重新录音";}
        if(!live(u,u->target))a=0;
        add(u,28,478,320,76,a,0,UI_ICON_NONE,label,states[u->voice_state]);
        if(u->voice_state==UI_VOICE_ERROR&&u->voice_text[0])
            add(u,372,478,320,76,A_VOICE_COPY,0,UI_ICON_NONE,"复制草稿","保留已识别的文字");
        else add(u,372,478,320,76,u->voice_state==UI_VOICE_READY&&u->voice_text[0]?A_VOICE_INSERT:0,0,UI_ICON_NONE,"插入终端","不自动回车");
        add(u,28,568,320,58,u->voice_state==UI_VOICE_READY?A_VOICE_PASTE:0,0,UI_ICON_NONE,"粘贴到草稿",NULL);
        add(u,372,568,320,58,A_VOICE_CANCEL,0,UI_ICON_NONE,"取消",NULL);
        break;}
    case UI_INPUT:
        strcpy(u->title,"输入与按键");
        row(u,108,A_NORMAL,0,"终端操作",u->profile==TERM_PROFILE_TERMINAL?"当前模式":"方向键、Tab、粘贴等通用操作");
        row(u,196,A_CLOUD,0,"Claude",u->profile==TERM_PROFILE_CLAUDE?"当前模式":"使用现有 Claude 快捷键");
        row(u,284,A_INSERT,0,"tty 输入",u->profile==TERM_PROFILE_TTY?"当前模式":"直接输入字符");
        row(u,372,A_DEFAULT,0,"新 Tab 默认模式",u->draft.default_profile<0?"沿用 Lua 配置":u->draft.default_profile==0?"终端操作":u->draft.default_profile==1?"Claude":"tty 输入");
        add(u,28,472,320,76,A_BINDINGS,0,UI_ICON_NONE,"有效快捷键",NULL);
        add(u,372,472,320,76,A_SAVE,0,UI_ICON_NONE,"保存默认模式",NULL);break;
    case UI_APPEARANCE:{
        char op[48];snprintf(op,sizeof(op),"%d%% 不透明度",u->draft.opacity);strcpy(u->title,"悬浮入口");
        row(u,120,A_SIDE,0,"停靠方向",u->draft.side?"右侧":"左侧");
        row(u,220,A_OPACITY,0,"静止透明度",op);
        row(u,320,A_EDGE,0,"外观",u->draft.edge_handle?"贴边短柄":"圆形按钮");
        row(u,420,A_HEIGHT,0,"重置高度","回到中部偏下");
        row(u,520,A_SAVE,0,"保存",NULL);break;}
    case UI_TABS:
        strcpy(u->title,"标签页");
        for(unsigned i=0;i<u->tab_count;i++){
            char title[64];snprintf(title,sizeof(title),"Tab %u%s",u->tabs[i].id,u->tabs[i].id==u->active?" · 当前":"");
            add(u,28+(i%2)*344,108+(i/2)*98,320,80,A_TAB_SELECT,(int)i,UI_ICON_NONE,title,u->tabs[i].profile==0?"终端操作":u->tabs[i].profile==1?"Claude":"tty 输入");
        }
        if(u->tab_count<8)row(u,520,A_TAB_NEW,0,"新建标签页",NULL);
        break;
    case UI_EDITOR:{
        char value[280];
        if(u->editor_field==A_TOKEN)snprintf(value,sizeof(value),"已输入 %u 个字符",(unsigned)strlen(u->edit));
        else snprintf(value,sizeof(value),"%.*s|%s",(int)u->cursor,u->edit,u->edit+u->cursor);
        strcpy(u->title,u->editor_field==A_HOST?"编辑对端 IP":u->editor_field==A_PORT?"编辑端口":"替换配对密钥");
        row(u,130,0,0,u->editor_field==A_TOKEN?"64 位密钥（内容隐藏）":"使用实体键盘输入",value);
        add(u,28,246,320,76,A_EDIT_CLEAR,0,UI_ICON_NONE,"清空",NULL);
        add(u,372,246,320,76,A_EDIT_PASTE,0,UI_ICON_NONE,"粘贴",NULL);
        add(u,28,360,320,76,A_EDIT_CANCEL,0,UI_ICON_NONE,"取消",NULL);
        add(u,372,360,320,76,A_EDIT_DONE,0,UI_ICON_NONE,"完成",NULL);
        break;}
    default:break;
    }
}
static void back(quick_ui_t *u){
    if(u->page==UI_VOICE&&u->voice_state==UI_VOICE_INSERTING){quick_ui_message(u,"正在插入，请稍候");return;}
    if(u->page==UI_VOICE){effect(u,UI_EFFECT_VOICE_CANCEL,0);quick_ui_close(u);}
    else if(u->page==UI_CONNECTION&&u->connection_parent==UI_VOICE){u->page=UI_VOICE;u->draft=u->saved;}
    else if(u->page==UI_EDITOR){memset(u->edit,0,sizeof(u->edit));u->page=u->editor_parent;}
    else if(u->page==UI_QUICK||u->page==UI_SETTINGS||u->page==UI_TABS)quick_ui_close(u);
    else {u->page=UI_SETTINGS;u->draft=u->saved;}
    u->status[0]=0;u->dirty=1;quick_ui_layout(u);
}
static void activate(quick_ui_t *u,int a,int value){
    if(u->page==UI_VOICE&&u->voice_state==UI_VOICE_INSERTING){quick_ui_message(u,"正在插入，请稍候");return;}
    if(!live(u,u->target)&&a!=A_BACK&&a!=A_CLOSE&&a!=A_VOICE_CANCEL&&a!=A_VOICE_COPY&&a!=A_VOICE_EXPAND&&a!=A_VOICE_COLLAPSE){
        if(quick_ui_voice_context(u))quick_ui_voice_target_lost(u);else quick_ui_close(u);
        return;
    }
    if(a!=A_VOICE_EXPAND&&a!=A_VOICE_COLLAPSE)u->status[0]=0;
    switch(a){
    case A_BACK:case A_CLOSE:back(u);return;
    case A_NORMAL:effect(u,UI_EFFECT_MODE_NORMAL,0);quick_ui_close(u);break;
    case A_INSERT:effect(u,UI_EFFECT_MODE_INSERT,0);quick_ui_close(u);break;
    case A_CLOUD:effect(u,UI_EFFECT_CLOUD,0);quick_ui_close(u);break;
    case A_REMOTE:u->page=UI_VOICE;voice_reset(u);u->voice_expanded=0;break;
    case A_CONNECTION:u->connection_parent=u->page;u->page=UI_CONNECTION;u->draft=u->saved;break;
    case A_SETTINGS:u->page=UI_SETTINGS;break;
    case A_INPUT:u->page=UI_INPUT;u->draft=u->saved;break;
    case A_APPEARANCE:u->page=UI_APPEARANCE;u->draft=u->saved;break;
    case A_TABS:u->page=UI_TABS;break;
    case A_PASTE:effect(u,UI_EFFECT_PASTE,0);quick_ui_close(u);break;
    case A_INTERRUPT:effect(u,UI_EFFECT_INTERRUPT,0);quick_ui_close(u);break;
    case A_BINDINGS:effect(u,UI_EFFECT_BINDINGS,0);quick_ui_close(u);break;
    case A_TAB_NEW:effect(u,UI_EFFECT_TAB_NEW,0);quick_ui_close(u);break;
    case A_TAB_SELECT:if(value>=0&&(unsigned)value<u->tab_count){u->target=u->tabs[value].id;effect(u,UI_EFFECT_TAB_SELECT,value);quick_ui_close(u);}break;
    case A_ENABLED:u->draft.companion_enabled=!u->draft.companion_enabled;break;
    case A_DEFAULT:u->draft.default_profile++;if(u->draft.default_profile>2)u->draft.default_profile=-1;break;
    case A_SIDE:u->draft.side=!u->draft.side;break;
    case A_OPACITY:u->draft.opacity+=5;if(u->draft.opacity>85)u->draft.opacity=30;break;
    case A_EDGE:u->draft.edge_handle=!u->draft.edge_handle;break;
    case A_HEIGHT:u->draft.height_permille=550;quick_ui_message(u,"保存后恢复默认高度");break;
    case A_TEST:effect(u,UI_EFFECT_VOICE_TEST,0);quick_ui_message(u,"正在测试 Mac 连接");break;
    case A_VOICE_START:voice_reset(u);u->voice_state=UI_VOICE_CONNECTING;effect(u,UI_EFFECT_VOICE_START,0);break;
    case A_VOICE_STOP:if(u->voice_state==UI_VOICE_RECORDING){u->voice_state=UI_VOICE_FINALIZING;effect(u,UI_EFFECT_VOICE_STOP,0);}break;
    case A_VOICE_CANCEL:back(u);return;
    case A_VOICE_INSERT:if(u->voice_state==UI_VOICE_READY&&u->voice_text[0]){u->voice_state=UI_VOICE_INSERTING;effect(u,UI_EFFECT_VOICE_INSERT,0);}break;
    case A_VOICE_PASTE:if(u->voice_state==UI_VOICE_READY)effect(u,UI_EFFECT_VOICE_PASTE,0);break;
    case A_VOICE_COPY:if(u->voice_state==UI_VOICE_ERROR&&u->voice_text[0])effect(u,UI_EFFECT_VOICE_COPY,0);break;
    case A_VOICE_EXPAND:u->voice_expanded=1;break;
    case A_VOICE_COLLAPSE:u->voice_expanded=0;break;
    case A_SAVE:
        if(!ui_settings_valid(&u->draft)){quick_ui_message(u,"请检查 IP 地址、端口及认证信息");break;}
        u->save_pending=1;effect(u,UI_EFFECT_SAVE,0);break;
    case A_HOST:case A_PORT:case A_TOKEN:
        u->editor_parent=u->page;u->page=UI_EDITOR;u->editor_field=a;u->alt=u->shift=u->sym=0;
        if(a==A_HOST)snprintf(u->edit,sizeof(u->edit),"%s",u->draft.companion_host);
        else if(a==A_PORT)snprintf(u->edit,sizeof(u->edit),"%d",u->draft.companion_port);
        else memset(u->edit,0,sizeof(u->edit));
        u->cursor=strlen(u->edit);break;
    case A_EDIT_CLEAR:memset(u->edit,0,sizeof(u->edit));u->cursor=0;break;
    case A_EDIT_PASTE:effect(u,UI_EFFECT_EDITOR_PASTE,0);break;
    case A_EDIT_CANCEL:back(u);return;
    case A_EDIT_DONE:{
        ui_settings_t s=u->draft;
        if(u->editor_field==A_HOST){if(strlen(u->edit)>=sizeof(s.companion_host)){quick_ui_message(u,"地址过长");break;}strcpy(s.companion_host,u->edit);}
        else if(u->editor_field==A_TOKEN)strcpy(s.companion_token,u->edit);
        else {char *end;long p=strtol(u->edit,&end,10);if(!*u->edit||*end||p<1||p>65535){quick_ui_message(u,"端口必须是 1–65535");break;}s.companion_port=(int)p;}
        int enabled=s.companion_enabled;s.companion_enabled=0;
        if(!ui_settings_valid(&s)){quick_ui_message(u,"无效内容，请检查后再保存");break;}s.companion_enabled=enabled;u->draft=s;
        memset(u->edit,0,sizeof(u->edit));u->page=u->editor_parent;break;}
    default:break;
    }
    u->dirty=1;quick_ui_layout(u);
}
int quick_ui_editor_paste(quick_ui_t *u,const char *text,size_t n){
    if(u->page!=UI_EDITOR||!text||n+strlen(u->edit)>=sizeof(u->edit))return -1;
    for(size_t i=0;i<n;i++)if((unsigned char)text[i]<33||(unsigned char)text[i]>126)return -1;
    size_t len=strlen(u->edit);memmove(u->edit+u->cursor+n,u->edit+u->cursor,len-u->cursor+1);
    memcpy(u->edit+u->cursor,text,n);u->cursor+=n;u->dirty=1;quick_ui_layout(u);return 0;
}
static int key(quick_ui_t *u,const key_event_t *k){
    int sym=k->sym;
    if(blocked(u,sym)){
        if(!k->pressed){u->blocked[sym>>3]&=~(1u<<(sym&7));return 1;}
        if(k->repeat)return 1;
    }
    if(u->page==UI_CLOSED)return 0;
    if(!k->pressed)return 1;
    block(u,sym);if(k->repeat)return 1;
    if(u->page==UI_VOICE&&u->voice_state==UI_VOICE_INSERTING){quick_ui_message(u,"正在插入，请稍候");return 1;}
    if(sym==KEYCODE_LEFT_ALT||sym==KEYCODE_RIGHT_ALT){u->alt=!u->alt;return 1;}
    if(sym==KEYCODE_LEFT_SHIFT||sym==KEYCODE_RIGHT_SHIFT){u->shift=!u->shift;return 1;}
    if(sym==KEYCODE_BB_SYM_KEY){if(u->alt){u->alt=0;back(u);}return 1;}
    if(sym==KEYCODE_ESCAPE){back(u);return 1;}
    if(u->page==UI_VOICE){
        if(sym==KEYCODE_RETURN||sym==KEYCODE_KP_ENTER){
            if(u->voice_state==UI_VOICE_RECORDING)activate(u,A_VOICE_STOP,0);
            else if(u->voice_state==UI_VOICE_READY)activate(u,A_VOICE_INSERT,0);
            return 1;
        }
        if(u->voice_state!=UI_VOICE_READY)return 1;
        size_t n=strlen(u->voice_text),p=u->voice_cursor;
        if(sym==KEYCODE_LEFT)u->voice_cursor=utf8_prev(u->voice_text,p);
        else if(sym==KEYCODE_RIGHT)u->voice_cursor=utf8_next(u->voice_text,p);
        else if(sym==KEYCODE_HOME)u->voice_cursor=0;
        else if(sym==KEYCODE_END)u->voice_cursor=n;
        else if(sym==KEYCODE_BACKSPACE||sym==KEYCODE_DELETE){
            size_t from=sym==KEYCODE_BACKSPACE?utf8_prev(u->voice_text,p):p;
            size_t to=sym==KEYCODE_DELETE?utf8_next(u->voice_text,p):p;
            if(from!=to){memmove(u->voice_text+from,u->voice_text+to,n-to+1);u->voice_cursor=from;u->voice_edited=1;}
        }else{
            if(k->modifiers&KEYMOD_CTRL)return 1;
            unsigned cp=k->unicode>0?(unsigned)k->unicode:(sym>=32&&sym<=126?(unsigned)sym:0);
            if(u->alt||(k->modifiers&KEYMOD_ALT)){
                const char *letters="qwertyuiopasdfghjklzxcvbnm0",*symbols="#123()_-+@*456/:;'\"789?!,.0";
                const char *found=sym>0&&sym<128?strchr(letters,sym):NULL;
                if(found)cp=(unsigned char)symbols[found-letters];
            }else if((u->shift||(k->modifiers&KEYMOD_SHIFT))&&cp>='a'&&cp<='z')cp-=32;
            u->alt=u->shift=0;
            if(cp>=32&&cp!=127&&cp<=0x10ffff&&!(cp>=0xd800&&cp<=0xdfff)){
                char b[4];size_t len;
                if(cp<0x80){b[0]=(char)cp;len=1;}
                else if(cp<0x800){b[0]=(char)(0xc0|(cp>>6));b[1]=(char)(0x80|(cp&63));len=2;}
                else if(cp<0x10000){b[0]=(char)(0xe0|(cp>>12));b[1]=(char)(0x80|((cp>>6)&63));b[2]=(char)(0x80|(cp&63));len=3;}
                else{b[0]=(char)(0xf0|(cp>>18));b[1]=(char)(0x80|((cp>>12)&63));b[2]=(char)(0x80|((cp>>6)&63));b[3]=(char)(0x80|(cp&63));len=4;}
                if(quick_ui_voice_paste(u,b,len))quick_ui_message(u,"文本无效或已达长度上限");
            }
        }
        u->dirty=1;quick_ui_layout(u);return 1;
    }
    if(u->page!=UI_EDITOR)return 1;
    size_t n=strlen(u->edit);
    if(sym==KEYCODE_RETURN||sym==KEYCODE_KP_ENTER){activate(u,A_EDIT_DONE,0);return 1;}
    if(sym==KEYCODE_LEFT){if(u->cursor)u->cursor--;u->dirty=1;quick_ui_layout(u);return 1;}
    if(sym==KEYCODE_RIGHT){if(u->cursor<n)u->cursor++;u->dirty=1;quick_ui_layout(u);return 1;}
    if(sym==KEYCODE_BACKSPACE){if(u->cursor){memmove(u->edit+u->cursor-1,u->edit+u->cursor,n-u->cursor+1);u->cursor--;u->dirty=1;quick_ui_layout(u);}return 1;}
    if(sym==KEYCODE_DELETE){if(u->cursor<n){memmove(u->edit+u->cursor,u->edit+u->cursor+1,n-u->cursor);u->dirty=1;quick_ui_layout(u);}return 1;}
    unsigned char ch=(unsigned char)sym;
    if(u->alt||(k->modifiers&KEYMOD_ALT)){
        /* Q20 hardware legends, matching the existing default altsym table. */
        const char *letters="qwertyuiopasdfghjklzxcvbnm0";
        const char *symbols="#123()_-+@*456/:;'\"789?!,.0";
        const char *p=strchr(letters,sym);
        if(p && (size_t)(p-letters)<strlen(symbols))ch=symbols[p-letters];
    } else if((u->shift||(k->modifiers&KEYMOD_SHIFT))&&ch>='a'&&ch<='z')ch-=32;
    u->alt=u->shift=0;
    if(sym>=33&&sym<=126){char c=(char)ch;if(quick_ui_editor_paste(u,&c,1))quick_ui_message(u,"输入长度已达上限");}
    return 1;
}
int quick_ui_event(quick_ui_t *u,const event_t *e){
    if(e->type==TERM_EVENT_KEY)return key(u,&e->as.key);
    if(e->type==TERM_EVENT_TOUCH_DOWN){
        int x=e->as.touch.x,y=e->as.touch.y;
        if(u->page==UI_CLOSED&&!hit(u->ball,x,y))return 0;
        u->pointer_owned=1;u->ball_pointer=u->page==UI_CLOSED;u->moved=0;
        u->down_x=x;u->down_y=y;u->offset_x=x-u->ball.x;u->offset_y=y-u->ball.y;u->down_item=-1;
        u->down_action=u->down_value=0;
        for(int i=0;i<u->item_count;i++)if(hit(u->items[i].rect,x,y)){
            u->down_item=i;u->down_action=u->items[i].action;u->down_value=u->items[i].value;break;
        }
        u->dirty=1;return 1;
    }
    if(e->type==TERM_EVENT_TOUCH_MOVE||e->type==TERM_EVENT_TOUCH_UP){
        if(!u->pointer_owned)return u->page!=UI_CLOSED;
        int x=e->as.touch.x,y=e->as.touch.y,dx=x-u->down_x,dy=y-u->down_y;
        int threshold=px(u,12);if(dx*dx+dy*dy>threshold*threshold)u->moved=1;
        if(u->ball_pointer&&u->moved){u->ball.x=clamp(x-u->offset_x,px(u,12),u->w-u->ball.w-px(u,12));u->ball.y=clamp(y-u->offset_y,px(u,84),u->usable_h-u->ball.h-px(u,32));u->dirty=1;}
        if(e->type==TERM_EVENT_TOUCH_UP){
            int was_ball=u->ball_pointer;u->pointer_owned=u->ball_pointer=0;
            if(was_ball){
                if(u->moved){int top=px(u,84),travel=u->usable_h-top-u->ball.h-px(u,32);u->draft=u->saved;u->draft.side=u->ball.x+u->ball.w/2>=u->w/2;u->draft.height_permille=travel>0?clamp((u->ball.y-top)*1000/travel,0,1000):0;u->target=u->active;u->save_pending=1;effect(u,UI_EFFECT_SAVE,0);}
                else quick_ui_open(u,UI_QUICK);
            } else if(!u->moved&&u->down_item>=0&&u->down_item<u->item_count&&hit(u->items[u->down_item].rect,x,y)){
                ui_item_t item=u->items[u->down_item];
                /* A partial/final update can replace an action between DOWN
                 * and UP. Never activate a newly enabled/replaced button. */
                if(item.action&&item.action==u->down_action&&item.value==u->down_value)
                    activate(u,item.action,item.value);
            }
            u->dirty=1;quick_ui_layout(u);
        }
        return 1;
    }
    return 0;
}
int quick_ui_take_effect(quick_ui_t *u,ui_effect_t *out){if(u->effect.kind==UI_EFFECT_NONE)return 0;*out=u->effect;u->effect.kind=UI_EFFECT_NONE;return 1;}
void quick_ui_save_result(quick_ui_t *u,int success){
    if(success){u->saved=u->draft;quick_ui_message(u,"设置已保存");}
    else quick_ui_message(u,"保存失败，保留原设置；草稿仍可重试");
    u->save_pending=0;quick_ui_layout(u);
}
