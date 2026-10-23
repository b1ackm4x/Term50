/* Host visual/layout regression. Build with FreeType; pass a CJK font path.
 * Optional second argument writes the ready composition as a PPM image. */
#include "ui_painter.h"
#include <sys/keycodes.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bitmap_t *blend_target;
static size_t blend_area;
static int blend_calls;
void renderer_blend_bitmap(renderer_t *r,int x,int y,const bitmap_t *b){
    (void)r;assert(b&&b->pixels&&b->w>0&&b->h>0);
    if(blend_target){
        assert(x>=0&&y>=0&&x+b->w<=blend_target->w&&y+b->h<=blend_target->h);
        bitmap_blend(blend_target,x,y,b);blend_area+=(size_t)b->w*b->h;blend_calls++;
    }
}
static int render(ui_painter_t *p,quick_ui_t *u,const char *font){
    rgb_t base={51,77,97};
    blend_target=bitmap_alloc(u->w,u->h,BITMAP_FMT_RGBA8888);assert(blend_target);
    bitmap_fill_rect(blend_target,NULL,base);blend_area=0;blend_calls=0;
    int repaint=u->dirty||!p->canvas||p->canvas->w!=u->w||p->canvas->h!=u->h;
    int result=ui_painter_render(p,u,NULL,font);assert(!result);
    bitmap_t *expected=bitmap_alloc(u->w,u->h,BITMAP_FMT_RGBA8888);assert(expected);
    bitmap_fill_rect(expected,NULL,base);bitmap_blend(expected,0,0,p->canvas);
    assert(!memcmp(blend_target->pixels,expected->pixels,(size_t)expected->stride*expected->h));
    bitmap_free(expected);bitmap_free(blend_target);blend_target=NULL;
    assert(blend_calls<=2);
    if(quick_ui_voice_inline(u)||u->page==UI_CLOSED)
        assert(blend_area<(size_t)u->w*u->h/3);
    /* Reuse after moves, shortening, panel changes and fades must equal a
     * fresh transparent canvas, rather than retaining old overlay pixels. */
    if(repaint){
        quick_ui_t copy=*u;ui_painter_t fresh=*p;fresh.canvas=NULL;fresh.painted_count=0;memset(fresh.glyphs,0,sizeof(fresh.glyphs));memset(fresh.metrics,0,sizeof(fresh.metrics));copy.dirty=1;
        assert(!ui_painter_render(&fresh,&copy,NULL,font));
        for(int y=0;y<u->h;y++)for(int x=0;x<u->w;x++){
            const unsigned char *a=p->canvas->pixels+(size_t)y*p->canvas->stride+x*4;
            const unsigned char *b=fresh.canvas->pixels+(size_t)y*fresh.canvas->stride+x*4;
            assert(a[3]==b[3]);if(a[3])assert(!memcmp(a,b,3));
        }
        for(int i=0;i<UI_PAINTER_GLYPH_CACHE_SIZE;i++)bitmap_free(fresh.glyphs[i].bitmap);
        bitmap_free(fresh.canvas);
    }
    return result;
}
static void press(quick_ui_t *u,int key){
    event_t e={.type=TERM_EVENT_KEY};e.as.key.sym=key;e.as.key.pressed=1;
    assert(quick_ui_event(u,&e));e.as.key.pressed=0;assert(quick_ui_event(u,&e));
}
static void click(quick_ui_t *u,const char *label){
    int found=-1;for(int i=0;i<u->item_count;i++)if(!strcmp(u->items[i].label,label))found=i;
    assert(found>=0);rect_t r=u->items[found].rect;
    event_t e={.type=TERM_EVENT_TOUCH_DOWN};e.as.touch.x=r.x+r.w/2;e.as.touch.y=r.y+r.h/2;
    assert(quick_ui_event(u,&e));e.type=TERM_EVENT_TOUCH_UP;assert(quick_ui_event(u,&e));
}
static unsigned long pixels_hash(const bitmap_t *b){
    unsigned long h=5381;for(int y=0;y<b->h;y++)for(int x=0;x<b->w*4;x++)h=(h*33)^b->pixels[y*b->stride+x];return h;
}
static void ppm(const char *path,const bitmap_t *b);
static void inline_frame(const char *dir,int frame,const ui_painter_t *p){
    if(!dir)return;
    bitmap_t *b=bitmap_alloc(p->canvas->w,p->canvas->h,BITMAP_FMT_RGBA8888);assert(b);
    bitmap_fill_rect(b,NULL,p->terminal_bg);
    const char *labels[]={"Term50","Wi-Fi connected","Voice test> "};
    for(int j=0;j<3;j++){
        int x=0,y=j==2?p->voice_y:32+j*p->cell_h;
        for(const char *s=labels[j];*s;s++){
            bitmap_t *g=font_render_glyph_cell(p->terminal_font,*s,FONT_STYLE_NORMAL,(rgb_t){192,199,231},p->terminal_bg,p->cell_w,p->cell_h);
            assert(g);bitmap_blit(b,x,y,g);bitmap_free(g);x+=p->cell_w;
        }
    }
    bitmap_blend(b,0,0,p->canvas);char path[1024];snprintf(path,sizeof(path),"%s/frame-%03d.ppm",dir,frame);ppm(path,b);bitmap_free(b);
}
static void ppm(const char *path,const bitmap_t *b){
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",b->w,b->h);
    for(int y=0;y<b->h;y++)for(int x=0;x<b->w;x++){
        const unsigned char *p=b->pixels+y*b->stride+x*4;
        unsigned char rgb[3]={p[2],p[1],p[0]};assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}
int main(int argc,char **argv){
    assert(argc>=2);assert(!font_library_init());
    ui_settings_t settings;ui_settings_defaults(&settings);strcpy(settings.companion_host,"192.0.2.10");
    quick_ui_t u;quick_ui_init(&u,&settings);quick_ui_geometry(&u,720,720,0);
    ui_tab_t tab={11,TERM_INPUT_INSERT,TERM_PROFILE_TTY};quick_ui_sync(&u,&tab,1,11);quick_ui_open(&u,UI_VOICE);
    click(&u,"展开");
    quick_ui_voice_update(&u,UI_VOICE_READY,"给终端增加语音输入。\n说话时逐字显示，结束后自动整理标点。\n确认内容后插入，不自动回车。",1,"识别完成，可用实体键盘编辑。");
    ui_painter_t p={0};int before_prepare_dirty=u.dirty;
    assert(!ui_painter_voice_prepare(&p,&u,argv[1]));assert(u.dirty==before_prepare_dirty&&u.page==UI_VOICE&&u.voice_expanded);
    assert(!render(&p,&u,argv[1]));assert(u.voice_scroll==0);assert(p.cjk);
    if(argc>2)ppm(argv[2],p.canvas);
    char long_text[UI_VOICE_TEXT_MAX+1]={0};
    for(int i=0;i<210;i++)strcat(long_text,"中文语音流式输入，长文本自动换行。\n");
    quick_ui_close(&u);quick_ui_open(&u,UI_VOICE);
    click(&u,"展开");
    quick_ui_voice_update(&u,UI_VOICE_READY,long_text,2,NULL);
    assert(!render(&p,&u,argv[1]));assert(u.voice_scroll>100);
    press(&u,KEYCODE_HOME);assert(!render(&p,&u,argv[1]));assert(u.voice_scroll==0);
    press(&u,KEYCODE_END);assert(!render(&p,&u,argv[1]));assert(u.voice_scroll>100);
    quick_ui_geometry(&u,720,720,220);assert(!render(&p,&u,argv[1]));
    assert(u.voice_rect.y+u.voice_rect.h<=u.usable_h);assert(u.voice_scroll>100);
    quick_ui_geometry(&u,720,720,0);quick_ui_close(&u);quick_ui_open(&u,UI_VOICE);
    font_t *terminal_font=font_open(argv[1],28);assert(terminal_font);
    ui_painter_voice_anchor(&p,&u,terminal_font,180,180,15,36,30,(rgb_t){26,27,38});
    unsigned long before=0;int frame=0;
    for(uint64_t t=10000;t<=12500;t+=100){
        if(t==10000)quick_ui_voice_update(&u,UI_VOICE_CONNECTING,NULL,0,NULL);
        if(t==10400)quick_ui_voice_update(&u,UI_VOICE_RECORDING,"",1,NULL);
        if(t==10600)quick_ui_voice_update(&u,UI_VOICE_RECORDING,"今",2,NULL);
        if(t==10800)quick_ui_voice_update(&u,UI_VOICE_RECORDING,"今天",3,NULL);
        if(t==11000)quick_ui_voice_update(&u,UI_VOICE_RECORDING,"今天下午三点",4,NULL);
        if(t==11200)quick_ui_voice_update(&u,UI_VOICE_RECORDING,"今天下午",5,NULL);
        if(t==11400)quick_ui_voice_update(&u,UI_VOICE_RECORDING,"今天下午三点",6,NULL);
        if(t==11800)quick_ui_voice_update(&u,UI_VOICE_FINALIZING,NULL,6,NULL);
        if(t==12200)quick_ui_voice_update(&u,UI_VOICE_READY,"今天下午3点，检查服务。",7,NULL);
        ui_painter_voice_tick(&p,&u,t,t<11200?0:800);
        assert(!render(&p,&u,argv[1]));
        if(t==10000)before=pixels_hash(p.canvas);
        if(t==10300)assert(before!=pixels_hash(p.canvas));
        if(argc>3)inline_frame(argv[3],frame++,&p);
    }
    size_t visible_pixels=0;
    for(int y=0;y<p.canvas->h;y++)for(int x=0;x<p.canvas->w;x++)visible_pixels+=p.canvas->pixels[y*p.canvas->stride+x*4+3]!=0;
    assert(visible_pixels<(size_t)p.canvas->w*p.canvas->h/5);
    assert(u.voice_rect.x==180&&u.voice_rect.y==180);
    /* Moving to the bottom moves the controls out of the composing rows. */
    ui_painter_voice_anchor(&p,&u,terminal_font,180,650,15,36,30,p.terminal_bg);
    assert(!render(&p,&u,argv[1]));
    assert(u.voice_capsule.y+u.voice_capsule.h<=u.voice_rect.y||u.voice_capsule.y>=u.voice_rect.y+u.voice_rect.h);
    /* A wrapped draft near the right edge needs a full-width overlap box. */
    quick_ui_close(&u);quick_ui_open(&u,UI_VOICE);
    quick_ui_voice_update(&u,UI_VOICE_READY,"中文语音输入需要自动换行并避开底部状态组件。",1,NULL);
    ui_painter_voice_anchor(&p,&u,terminal_font,630,650,15,36,30,p.terminal_bg);
    ui_painter_voice_tick(&p,&u,12700,0);
    assert(!render(&p,&u,argv[1]));
    assert(u.voice_rect.x==0&&u.voice_rect.h>36);
    assert(u.voice_capsule.y+u.voice_capsule.h<=u.voice_rect.y);
    ui_painter_voice_anchor(&p,&u,terminal_font,180,180,15,36,30,p.terminal_bg);
    quick_ui_close(&u);ui_painter_voice_complete(&p,13000);
    for(uint64_t t=13000;t<=14300;t+=100){
        ui_painter_voice_tick(&p,&u,t,0);assert(!render(&p,&u,argv[1]));
        if(argc>3)inline_frame(argv[3],frame++,&p);
    }
    assert(!p.complete_ms);
    /* Stable capsule labels and a stable ASCII draft must reuse rasterized
     * glyphs; no FreeType work is necessary on a forced animation redraw. */
    quick_ui_open(&u,UI_VOICE);quick_ui_voice_update(&u,UI_VOICE_RECORDING,"aaaaaaaa",1,NULL);
    ui_painter_voice_anchor(&p,&u,terminal_font,180,180,15,36,30,p.terminal_bg);
    ui_painter_voice_tick(&p,&u,15000,0);assert(!render(&p,&u,argv[1]));
    ui_painter_voice_tick(&p,&u,15300,0);assert(!render(&p,&u,argv[1]));
    uint64_t renders=p.glyph_renders,hits=p.glyph_hits;u.dirty=1;
    assert(!render(&p,&u,argv[1]));assert(p.glyph_hits>hits&&p.glyph_renders==renders);
    /* Changing borrowed font identity or terminal dimensions drops old
     * entries even when its old allocation remains alive during the swap. */
    uint32_t epoch=p.glyph_epoch;unsigned long original=pixels_hash(p.canvas);
    font_t *alternate=font_open(argv[1],20);assert(alternate);
    ui_painter_voice_anchor(&p,&u,alternate,180,180,15,36,30,p.terminal_bg);
    assert(!render(&p,&u,argv[1]));assert(p.glyph_epoch!=epoch&&pixels_hash(p.canvas)!=original);
    epoch=p.glyph_epoch;ui_painter_voice_anchor(&p,&u,alternate,180,180,16,38,30,p.terminal_bg);
    assert(!render(&p,&u,argv[1]));assert(p.glyph_epoch!=epoch);
    ui_painter_voice_anchor(&p,&u,terminal_font,180,180,15,36,30,p.terminal_bg);
    assert(!render(&p,&u,argv[1]));font_close(alternate);
    /* Owned UI fonts can be resized before capture with no UI side effects. */
    epoch=p.glyph_epoch;quick_ui_geometry(&u,720,720,220);ui_page_t previous_page=u.page;
    assert(!ui_painter_voice_prepare(&p,&u,argv[1]));assert(p.glyph_epoch!=epoch&&u.page==previous_page);
    for(int i=0;i<UI_PAINTER_GLYPH_CACHE_SIZE;i++)assert(!p.glyphs[i].bitmap&&!p.metrics[i].advance);
    assert(!render(&p,&u,argv[1]));
    quick_ui_geometry(&u,720,720,0);assert(!ui_painter_voice_prepare(&p,&u,argv[1]));assert(!render(&p,&u,argv[1]));
    ui_painter_destroy(&p);font_close(terminal_font);font_library_quit();puts("voice painter: transparency, anchor, avoidance, animation, fade, wrap, resize, crop equivalence and glyph cache lifecycle passed");
    return 0;
}
