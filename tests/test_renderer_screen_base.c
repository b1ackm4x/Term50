/* Exercises the full production native Screen renderer against two padded,
 * alternating buffers. Only OS handles/font/menu services are controlled. */
#include "renderer_screen.h"
#include "platform_screen.h"
#include "symmenu_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
struct fake_screen_buffer { unsigned char *pixels; int w,h,stride; };
static struct fake_screen_buffer buffers[2];
static int current, posts, post_rect[4], fail_window, fail_pointer;
static bitmap_t *owned[32];
static unsigned allocations,frees;

/* Only the renderer translation unit redirects allocation/free to these
 * observers; real bitmap storage and all copy/blend operations stay real. */
bitmap_t *test_bitmap_alloc(int w,int h,bitmap_fmt_t fmt){
    bitmap_t *b=bitmap_alloc(w,h,fmt);
    if(b){unsigned i=0;while(i<32&&owned[i])i++;CHECK(i<32);owned[i]=b;allocations++;}
    return b;
}
void test_bitmap_free(bitmap_t *b){
    if(b){unsigned i=0;while(i<32&&owned[i]!=b)i++;CHECK(i<32);owned[i]=NULL;frees++;}
    bitmap_free(b);
}
static unsigned live(void){unsigned n=0;for(unsigned i=0;i<32;i++)n+=owned[i]!=NULL;return n;}

screen_context_t platform_screen_context(platform_t *p){return p?(screen_context_t)buffers:NULL;}
screen_window_t platform_screen_window(platform_t *p){return p?(screen_window_t)buffers:NULL;}
int screen_get_window_property_pv(screen_window_t w,int prop,void **out){
    CHECK(w&&prop==SCREEN_PROPERTY_RENDER_BUFFERS);if(fail_window)return -1;
    out[0]=&buffers[current];out[1]=&buffers[1-current];return 0;
}
int screen_get_window_property_iv(screen_window_t w,int prop,int *out){
    CHECK(w);if(prop==SCREEN_PROPERTY_RENDER_BUFFER_COUNT){*out=2;return 0;}
    CHECK(prop==SCREEN_PROPERTY_SIZE);out[0]=buffers[current].w;out[1]=buffers[current].h;return 0;
}
int screen_get_buffer_property_pv(screen_buffer_t b,int prop,void **out){
    CHECK(b&&prop==SCREEN_PROPERTY_POINTER);*out=fail_pointer?NULL:b->pixels;return 0;
}
int screen_get_buffer_property_iv(screen_buffer_t b,int prop,int *out){
    CHECK(b);if(prop==SCREEN_PROPERTY_STRIDE){*out=b->stride;return 0;}
    CHECK(prop==SCREEN_PROPERTY_BUFFER_SIZE);out[0]=b->w;out[1]=b->h;return 0;
}
int screen_post_window(screen_window_t w,screen_buffer_t b,int n,const int *rect,int flags){
    CHECK(w&&b==&buffers[current]&&n==1&&flags==0);memcpy(post_rect,rect,sizeof(post_rect));posts++;current=1-current;return 0;
}
bitmap_t *font_render_glyph_cell(font_t *f,uint32_t cp,font_style_t style,rgb_t fg,rgb_t bg,int w,int h){
    (void)f;(void)cp;(void)style;(void)fg;(void)bg;(void)w;(void)h;return NULL;
}
symmenu_render_t *symmenu_render_create(int w,int h,pref_t *p,symmenu_t *m){(void)w;(void)h;(void)p;(void)m;return NULL;}
void symmenu_render_destroy(symmenu_render_t *m){CHECK(!m);}

static void resize(int w,int h,int padding){
    for(int i=0;i<2;i++){
        free(buffers[i].pixels);buffers[i].w=w;buffers[i].h=h;buffers[i].stride=w*4+padding;
        buffers[i].pixels=malloc((size_t)buffers[i].stride*h);CHECK(buffers[i].pixels);
        memset(buffers[i].pixels,0x6b,(size_t)buffers[i].stride*h);
    }
    current=0;
}
static unsigned char *snapshot(void){
    struct fake_screen_buffer *b=&buffers[current];unsigned char *copy=malloc((size_t)b->w*b->h*4);CHECK(copy);
    for(int y=0;y<b->h;y++)memcpy(copy+(size_t)y*b->w*4,b->pixels+(size_t)y*b->stride,(size_t)b->w*4);
    return copy;
}
static void equals(const unsigned char *expected){
    struct fake_screen_buffer *b=&buffers[current];
    for(int y=0;y<b->h;y++)CHECK(!memcmp(b->pixels+(size_t)y*b->stride,expected+(size_t)y*b->w*4,(size_t)b->w*4));
}
static void padding_untouched(void){
    for(int i=0;i<2;i++)for(int y=0;y<buffers[i].h;y++)
        for(int x=buffers[i].w*4;x<buffers[i].stride;x++)CHECK(buffers[i].pixels[(size_t)y*buffers[i].stride+x]==0x6b);
}
static void full_dirty(int w,int h){CHECK(post_rect[0]==0&&post_rect[1]==0&&post_rect[2]==w&&post_rect[3]==h);}
static rect_t clipped(rect_t r,int w,int h){
    int right=r.x+r.w,bottom=r.y+r.h;
    if(r.x<0)r.x=0;if(r.y<0)r.y=0;
    if(right>w)right=w;if(bottom>h)bottom=h;
    r.w=right-r.x;r.h=bottom-r.y;
    if(r.w<0||r.h<0)return (rect_t){0};
    return r;
}
static void dirty_union(rect_t old,rect_t next,int w,int h){
    old=clipped(old,w,h);next=clipped(next,w,h);
    if(!old.w||!old.h)old=next;
    if(!next.w||!next.h)next=old;
    int x=old.x<next.x?old.x:next.x,y=old.y<next.y?old.y:next.y;
    int right=old.x+old.w>next.x+next.w?old.x+old.w:next.x+next.w;
    int bottom=old.y+old.h>next.y+next.h?old.y+old.h:next.y+next.h;
    CHECK(post_rect[0]==x&&post_rect[1]==y&&post_rect[2]==right-x&&post_rect[3]==bottom-y);
}
static void check_blended(const unsigned char *base,const bitmap_t *overlay,int x,int y){
    struct fake_screen_buffer *b=&buffers[current];
    for(int row=0;row<b->h;row++)for(int col=0;col<b->w;col++){
        const unsigned char *before=base+((size_t)row*b->w+col)*4;
        const unsigned char *actual=b->pixels+(size_t)row*b->stride+col*4;
        if(col>=x&&col<x+overlay->w&&row>=y&&row<y+overlay->h){
            const unsigned char *src=overlay->pixels+(size_t)(row-y)*overlay->stride+(col-x)*4;
            unsigned alpha=src[3];
            for(int c=0;c<3;c++)CHECK(actual[c]==(src[c]*alpha+before[c]*(255-alpha)+127)/255);
            CHECK(actual[3]==255);
        }else CHECK(!memcmp(actual,before,4));
    }
}
int main(void){
    CHECK(!renderer_screen_capture_base(NULL)&&!renderer_screen_restore_base(NULL));
    resize(7,5,12);
    renderer_t *r=renderer_screen_create((platform_t *)buffers,NULL);CHECK(r);
    renderer_begin_frame(r);CHECK(!renderer_screen_restore_base(r));
    CHECK(!posts);renderer_end_frame(r,0);CHECK(!posts);CHECK(!live());
    renderer_begin_frame(r);renderer_clear(r,(rgb_t){13,27,41});
    rect_t marker={2,1,3,2};renderer_fill_rect(r,&marker,(rgb_t){104,8,196});
    CHECK(renderer_screen_capture_base(r));CHECK(live()==1);
    unsigned char *base=snapshot();renderer_end_frame(r,1);full_dirty(7,5);
    bitmap_t *overlay=bitmap_alloc(3,2,BITMAP_FMT_RGBA8888);CHECK(overlay);
    for(int y=0;y<2;y++)for(int x=0;x<3;x++){
        unsigned char *p=overlay->pixels+(size_t)y*overlay->stride+x*4;
        p[0]=216;p[1]=67;p[2]=145;p[3]=x==0?0:x==1?128:221;
    }
    /* Alternate both backing buffers while moving translucent controls. Each
     * restore must erase the previous frame, including its former position. */
    rect_t old_overlay[2]={{0},{0}};int base_seen[2]={1,0};
    for(int frame=0;frame<12;frame++){
        int slot=current,first_generation=!base_seen[slot];
        renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);
        int x=frame%4,y=frame%3;renderer_blend_bitmap(r,x,y,overlay);check_blended(base,overlay,x,y);
        int before=posts;renderer_end_frame(r,0);CHECK(posts==before+1);
        if(first_generation)full_dirty(7,5);
        else {dirty_union(old_overlay[slot],(rect_t){x,y,3,2},7,5);CHECK(post_rect[2]*post_rect[3]<7*5);}
        base_seen[slot]=1;old_overlay[slot]=(rect_t){x,y,3,2};
        padding_untouched();CHECK(live()==1);
    }
    /* A shell repaint becomes the new owned base, rather than restoring the
     * first captured terminal content forever. */
    renderer_begin_frame(r);renderer_clear(r,(rgb_t){209,71,3});
    marker=(rect_t){1,3,5,1};renderer_fill_rect(r,&marker,(rgb_t){7,77,177});
    CHECK(renderer_screen_capture_base(r));free(base);base=snapshot();CHECK(live()==1);
    renderer_blend_bitmap(r,0,0,overlay);renderer_end_frame(r,0);
    renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);renderer_end_frame(r,0);full_dirty(7,5);
    renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);renderer_end_frame(r,0);
    dirty_union((rect_t){0,0,3,2},(rect_t){0},7,5);
    /* A resized latch cannot use an old-size capture. Fallback must leave
     * framebuffer bytes and dirty state untouched until the caller repaints. */
    resize(9,3,20);renderer_begin_frame(r);unsigned char *untouched=snapshot();int before_posts=posts;
    CHECK(!renderer_screen_restore_base(r));equals(untouched);renderer_end_frame(r,0);CHECK(posts==before_posts);free(untouched);
    renderer_begin_frame(r);renderer_clear(r,(rgb_t){44,55,66});CHECK(renderer_screen_capture_base(r));
    free(base);base=snapshot();CHECK(live()==1);renderer_end_frame(r,1);
    renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);renderer_end_frame(r,0);full_dirty(9,3);padding_untouched();
    /* More than four independent blends must remain safe. In that case the
     * backend remembers a full overlay region instead of forgetting one. */
    renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);
    int crowded_slot=current;
    for(int i=0;i<9;i++)renderer_blend_bitmap(r,i%7,i%2,overlay);
    renderer_end_frame(r,0);
    renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);
    renderer_blend_bitmap(r,6,0,overlay);renderer_end_frame(r,0);
    CHECK(current==crowded_slot);
    renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);renderer_end_frame(r,0);full_dirty(9,3);
    /* Clip both draw and saved regions at every edge. Offscreen blends must
     * neither post an empty dirty box nor leave an invalid region to restore. */
    current=crowded_slot;renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);
    int before_clip=posts;renderer_blend_bitmap(r,-12,-12,overlay);renderer_end_frame(r,0);CHECK(posts==before_clip);
    renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));renderer_blend_bitmap(r,-2,-1,overlay);
    check_blended(base,overlay,-2,-1);renderer_end_frame(r,0);dirty_union((rect_t){0},(rect_t){-2,-1,3,2},9,3);
    current=crowded_slot;renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);
    renderer_blend_bitmap(r,8,2,overlay);check_blended(base,overlay,8,2);renderer_end_frame(r,0);
    dirty_union((rect_t){-2,-1,3,2},(rect_t){8,2,3,2},9,3);
    current=crowded_slot;renderer_begin_frame(r);CHECK(renderer_screen_restore_base(r));equals(base);
    renderer_end_frame(r,0);dirty_union((rect_t){8,2,3,2},(rect_t){0},9,3);padding_untouched();
    fail_pointer=1;renderer_begin_frame(r);CHECK(!renderer_screen_capture_base(r)&&!renderer_screen_restore_base(r));
    before_posts=posts;renderer_end_frame(r,0);CHECK(posts==before_posts);fail_pointer=0;
    fail_window=1;renderer_begin_frame(r);CHECK(!renderer_screen_capture_base(r)&&!renderer_screen_restore_base(r));renderer_end_frame(r,0);fail_window=0;
    renderer_destroy(r);CHECK(!live()&&allocations==frees);
    bitmap_free(overlay);free(base);for(int i=0;i<2;i++)free(buffers[i].pixels);
    printf("renderer Screen base: %u checks passed (production backend, alternating padded buffers)\n",checks);return 0;
}
