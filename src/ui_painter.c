#include "ui_painter.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unicode/utf8.h>
#include <unicode/uchar.h>

static const rgb_t panel={24,31,27},button={37,47,40},ink={231,240,233},muted={169,185,174},accent={142,212,165},dark={19,42,27};
static int unit(const quick_ui_t *u,int x){return x*u->scale/1000;}
static uint32_t painter_color(rgb_t c){return ((uint32_t)c.r<<16)|((uint32_t)c.g<<8)|c.b;}
static uint32_t painter_font_hash(font_t *f){uintptr_t v=(uintptr_t)f;return (uint32_t)(v^(v>>16));}
static void painter_glyphs_clear(ui_painter_t *p){
    for(int i=0;i<UI_PAINTER_GLYPH_CACHE_SIZE;i++)bitmap_free(p->glyphs[i].bitmap);
    memset(p->glyphs,0,sizeof(p->glyphs));memset(p->metrics,0,sizeof(p->metrics));
    if(++p->glyph_epoch==0)p->glyph_epoch=1;
}
static void painter_terminal_context(ui_painter_t *p){
    if(p->cached_terminal_font==p->terminal_font&&p->cached_cell_w==p->cell_w&&p->cached_cell_h==p->cell_h)return;
    painter_glyphs_clear(p);p->cached_terminal_font=p->terminal_font;
    p->cached_cell_w=p->cell_w;p->cached_cell_h=p->cell_h;
}
/* Entries own rasterized glyphs. Callers only blit them; eviction and font
 * changes release old bitmaps. Dimensions zero select shaded text metrics. */
static const bitmap_t *painter_glyph(ui_painter_t *p,font_t *f,uint32_t cp,rgb_t fg,rgb_t bg,int cw,int ch){
    uint32_t foreground=painter_color(fg),background=painter_color(bg);
    uint32_t hash=painter_font_hash(f)^cp*2654435761u;
    hash=(hash^foreground)*16777619u;hash=(hash^background)*16777619u;
    hash=(hash^(uint32_t)cw)*16777619u;hash=(hash^(uint32_t)ch)*16777619u;
    ui_painter_glyph_t *g=&p->glyphs[hash&(UI_PAINTER_GLYPH_CACHE_SIZE-1)];
    if(g->bitmap&&g->font==f&&g->codepoint==cp&&g->fg==foreground&&g->bg==background&&
       g->cell_w==cw&&g->cell_h==ch&&g->epoch==p->glyph_epoch){p->glyph_hits++;return g->bitmap;}
    bitmap_t *b=cw||ch?font_render_glyph_cell(f,cp,FONT_STYLE_NORMAL,fg,bg,cw,ch):
        font_render_glyph_shaded(f,cp,FONT_STYLE_NORMAL,fg,bg);
    if(!b)return NULL;
    p->glyph_renders++;
    bitmap_free(g->bitmap);*g=(ui_painter_glyph_t){f,b,cp,foreground,background,p->glyph_epoch,cw,ch};
    return b;
}
static void pixel(bitmap_t *b,int x,int y,rgb_t c,int alpha){
    if(x<0||y<0||x>=b->w||y>=b->h)return;
    unsigned char *p=b->pixels+(size_t)y*b->stride+x*4;
    p[0]=c.b;p[1]=c.g;p[2]=c.r;p[3]=(unsigned char)alpha;
}
static void round_rect(bitmap_t *b,rect_t r,int radius,rgb_t c,int alpha){
    int rr=radius*radius;
    for(int y=0;y<r.h;y++)for(int x=0;x<r.w;x++){
        int dx=x<radius?radius-x-1:x>=r.w-radius?x-(r.w-radius):0;
        int dy=y<radius?radius-y-1:y>=r.h-radius?y-(r.h-radius):0;
        if(dx*dx+dy*dy<=rr)pixel(b,r.x+x,r.y+y,c,alpha);
    }
}
static void line(bitmap_t *b,int x0,int y0,int x1,int y1,int width,rgb_t c,int a){
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
    for(;;){round_rect(b,(rect_t){x0-width/2,y0-width/2,width,width},width/2,c,a);if(x0==x1&&y0==y1)break;int e=2*err;if(e>=dy){err+=dy;x0+=sx;}if(e<=dx){err+=dx;y0+=sy;}}
}
static void icon(bitmap_t *b,rect_t r,ui_icon_t id,rgb_t c,int a){
    int cx=r.x+r.w/2,cy=r.y+r.h/2,s=r.h*26/100,w=r.h/22;if(w<2)w=2;
    if(id==UI_ICON_TERMINAL){
        line(b,cx-s,cy-s/2,cx-s/3,cy,w,c,a);line(b,cx-s/3,cy,cx-s,cy+s/2,w,c,a);line(b,cx,cy+s/2,cx+s,cy+s/2,w,c,a);
    }else if(id==UI_ICON_CLOUD){
        /* Claude/Agent asterisk, not a network-cloud status indicator. */
        for(int i=0;i<8;i++){double t=i*3.141592653589793/4;line(b,cx+(int)(cos(t)*s*.35),cy+(int)(sin(t)*s*.35),cx+(int)(cos(t)*s),cy+(int)(sin(t)*s),w,c,a);}
    }else if(id==UI_ICON_KEYBOARD){
        line(b,cx-s,cy-s/2,cx+s,cy-s/2,w,c,a);line(b,cx-s,cy+s/2,cx+s,cy+s/2,w,c,a);
        line(b,cx-s,cy-s/2,cx-s,cy+s/2,w,c,a);line(b,cx+s,cy-s/2,cx+s,cy+s/2,w,c,a);
        for(int j=0;j<2;j++)for(int i=0;i<4;i++)round_rect(b,(rect_t){cx-s*3/4+i*s/2,cy-s/4+j*s/3,w,w},0,c,a);
    }else if(id==UI_ICON_BACK){line(b,cx+s/2,cy-s/2,cx-s/2,cy,w,c,a);line(b,cx-s/2,cy,cx+s/2,cy+s/2,w,c,a);}
}
static int has_cjk(font_t *f){return f&&font_glyph_metrics(f,0x4e2d,NULL,NULL,NULL,NULL,NULL)==0&&font_glyph_metrics(f,0x8bbe,NULL,NULL,NULL,NULL,NULL)==0;}
static int find_font(const char *root,int depth,int *budget,char *out,size_t cap){
    if(depth<0||*budget<=0)return 0;
    DIR *d=opendir(root);if(!d)return 0;struct dirent *e;int found=0;
    while(!found&&(e=readdir(d))&&*budget>0){
        if(e->d_name[0]=='.')continue;
        char path[1024];int n=snprintf(path,sizeof(path),"%s/%s",root,e->d_name);if(n<0||n>=(int)sizeof(path))continue;
        struct stat st;if(stat(path,&st))continue;
        if(S_ISDIR(st.st_mode)){found=find_font(path,depth-1,budget,out,cap);continue;}
        const char *ext=strrchr(e->d_name,'.');
        if(!ext||(strcmp(ext,".ttf")&&strcmp(ext,".ttc")&&strcmp(ext,".otf")))continue;
        (*budget)--;font_t *f=font_open(path,28);if(has_cjk(f)){snprintf(out,cap,"%s",path);found=1;}font_close(f);
    }
    closedir(d);return found;
}
static int ensure_font(ui_painter_t *p,int size,const char *fallback){
    if(!p->font_path[0]){
        int budget=400;
        if(!find_font("/usr/fonts",4,&budget,p->font_path,sizeof(p->font_path)))
            snprintf(p->font_path,sizeof(p->font_path),"%s",fallback?fallback:"");
    }
    if(p->font_size==size&&p->font)return 0;
    font_t *f=font_open(p->font_path,size),*small=font_open(p->font_path,size*4/5);
    if(!f||!small){font_close(f);font_close(small);return -1;}
    painter_glyphs_clear(p);font_close(p->font);font_close(p->small);p->font=f;p->small=small;p->font_size=size;p->cjk=has_cjk(f);return 0;
}
static void painter_text(ui_painter_t *p,bitmap_t *b,font_t *f,int x,int y,int maxwidth,const char *str,rgb_t fg,rgb_t bg){
    int32_t i=0,len=(int32_t)strlen(str);int start=x;
    while(i<len){UChar32 cp;U8_NEXT(str,i,len,cp);if(cp<0)cp='?';
        const bitmap_t *glyph=painter_glyph(p,f,(uint32_t)cp,fg,bg,0,0);if(!glyph)continue;
        if(x+glyph->w>start+maxwidth)break;
        bitmap_blit(b,x,y,glyph);x+=glyph->w;
    }
}
static int voice_advance(ui_painter_t *p,font_t *f,UChar32 cp){
    uint32_t hash=painter_font_hash(f)^(uint32_t)cp*2654435761u;
    ui_painter_metric_t *m=&p->metrics[hash&(UI_PAINTER_GLYPH_CACHE_SIZE-1)];
    if(m->font==f&&m->codepoint==(uint32_t)cp&&m->epoch==p->glyph_epoch&&m->advance>0)return m->advance;
    int w=0;
    if(cp=='\t'){
        if(font_glyph_metrics(f,' ',NULL,NULL,NULL,NULL,&w)||w<1)w=8;
        w*=4;
    }else if(font_glyph_metrics(f,(uint32_t)cp,NULL,NULL,NULL,NULL,&w)||w<1)w=font_line_skip(f)/2;
    if(w<1)w=1;
    *m=(ui_painter_metric_t){f,(uint32_t)cp,p->glyph_epoch,w};return w;
}
static void voice_preview(ui_painter_t *p,quick_ui_t *u){
    rect_t r=u->voice_rect;int pad=unit(u,18),width=r.w-pad*2;
    round_rect(p->canvas,r,unit(u,12),button,255);
    painter_text(p,p->canvas,p->small,r.x+pad,r.y+unit(u,10),width,
        u->voice_state==UI_VOICE_READY?"草稿 · 左右键移动，确认后插入":u->voice_state==UI_VOICE_ERROR?"保留的草稿 · 可复制后继续使用":"实时草稿 · 结束录音后整理",muted,button);
    int top=r.y+unit(u,46),height=r.h-unit(u,76),lh=font_line_skip(p->font)+unit(u,4);
    if(lh<1||width<1||height<lh)return;
    int visible=height/lh;
    if(!u->voice_text[0]){
        painter_text(p,p->canvas,p->small,r.x+pad,top,width,"说话时，识别文字会在这里逐步显示。",muted,button);
        return;
    }
    size_t len=strlen(u->voice_text);
    /* One bounded layout pass; only visible lines rasterize glyphs. */
    size_t *starts=malloc((len+2)*sizeof(*starts));
    if(!starts){painter_text(p,p->canvas,p->small,r.x+pad,top,width,"暂时无法显示草稿",muted,button);return;}
    starts[0]=0;int count=1,x=0,caret_line=0,caret_x=0;
    int32_t i=0;
    while(i<(int32_t)len){
        int32_t start=i;UChar32 cp;U8_NEXT(u->voice_text,i,(int32_t)len,cp);
        if(cp<0)cp='?';
        int w=cp=='\n'?0:voice_advance(p,p->font,cp);
        if(cp!='\n'&&x&&x+w>width){starts[count++]=(size_t)start;x=0;}
        if((size_t)start==u->voice_cursor){caret_line=count-1;caret_x=x;}
        if(cp=='\n'){starts[count++]=(size_t)i;x=0;}else x+=w;
    }
    if(u->voice_cursor==len){caret_line=count-1;caret_x=x;}
    if(u->voice_scroll>caret_line)u->voice_scroll=caret_line;
    if(u->voice_scroll+visible<=caret_line)u->voice_scroll=caret_line-visible+1;
    if(u->voice_scroll<0)u->voice_scroll=0;
    starts[count]=len;
    for(int row=u->voice_scroll;row<count&&row<u->voice_scroll+visible;row++){
        int y=top+(row-u->voice_scroll)*lh;int32_t off=(int32_t)starts[row],end=(int32_t)starts[row+1];x=0;
        while(off<end){UChar32 cp;U8_NEXT(u->voice_text,off,end,cp);if(cp<0)cp='?';if(cp=='\n')break;
            int w=voice_advance(p,p->font,cp);
            if(cp!='\t'){
                const bitmap_t *glyph=painter_glyph(p,p->font,(uint32_t)cp,ink,button,0,0);
                if(glyph&&x+glyph->w<=width)bitmap_blit(p->canvas,r.x+pad+x,y,glyph);
            }
            x+=w;
        }
        if(row==caret_line&&u->voice_state==UI_VOICE_READY){
            int cx=r.x+pad+(caret_x<width?caret_x:width-2);
            line(p->canvas,cx,y,cx,y+font_line_skip(p->font)-1,unit(u,2)>0?unit(u,2):1,accent,255);
        }
    }
    char position[80];snprintf(position,sizeof(position),"%d / %d 行%s",caret_line+1,count,u->voice_edited?" · 已手动修改":"");
    painter_text(p,p->canvas,p->small,r.x+pad,r.y+r.h-unit(u,28),width,position,muted,button);
    free(starts);
}
#define text(...) painter_text(p,__VA_ARGS__)
#include "ui_voice_painter.inc"
#undef text
int ui_painter_voice_prepare(ui_painter_t *p,const quick_ui_t *u,const char *fallback_font){
    return ensure_font(p,unit(u,28),fallback_font);
}
static rect_t painter_clip(const bitmap_t *b,rect_t r){
    int x=r.x<0?0:r.x,y=r.y<0?0:r.y;
    int right=r.x+r.w>b->w?b->w:r.x+r.w,bottom=r.y+r.h>b->h?b->h:r.y+r.h;
    return right>x&&bottom>y?(rect_t){x,y,right-x,bottom-y}:(rect_t){0};
}
static int painter_overlap(rect_t a,rect_t b){
    return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;
}
static rect_t painter_union(rect_t a,rect_t b){
    int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
    int right=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w;
    int bottom=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
    return (rect_t){x,y,right-x,bottom-y};
}
static void painter_clear(ui_painter_t *p){
    for(int i=0;i<p->painted_count;i++){
        rect_t r=p->painted[i];
        for(int y=r.y;y<r.y+r.h;y++)
            memset(p->canvas->pixels+(size_t)y*p->canvas->stride+r.x*4,0,(size_t)r.w*4);
    }
    p->painted_count=0;
}
/* Trim known drawing regions against alpha, so rounded corners and an empty
 * composing row cannot turn a compact overlay into a full-screen blend. */
static rect_t painter_alpha_bounds(const bitmap_t *b,rect_t r){
    r=painter_clip(b,r);
    int left=r.x+r.w,right=r.x,top=r.y+r.h,bottom=r.y;
    for(int y=r.y;y<r.y+r.h;y++){
        const unsigned char *row=b->pixels+(size_t)y*b->stride;
        int first=r.x,last=r.x+r.w-1;
        while(first<=last&&!row[first*4+3])first++;
        if(first>last)continue;
        while(last>first&&!row[last*4+3])last--;
        if(first<left)left=first;if(last+1>right)right=last+1;
        if(y<top)top=y;bottom=y+1;
    }
    return right>left&&bottom>top?(rect_t){left,top,right-left,bottom-top}:(rect_t){0};
}
static void painter_bounds(ui_painter_t *p,const quick_ui_t *u){
    rect_t regions[2];int count=0;
    if(quick_ui_voice_inline(u)){
        if(u->voice_rect.w&&u->voice_rect.h){
            rect_t r=u->voice_rect;regions[count++]=(rect_t){r.x-2,r.y-2,r.w+4,r.h+4};
        }
        /* Capsule entrance rises six pixels; include its border as well. */
        rect_t r=u->voice_capsule;regions[count++]=(rect_t){r.x-2,r.y-2,r.w+4,r.h+10};
    }else if(u->page==UI_CLOSED&&p->complete_ms){
        int w=unit(u,220),h=unit(u,56);
        regions[count++]=(rect_t){(u->w-w)/2-2,u->usable_h-h-unit(u,24)-2,w+4,h+4};
    }else if(u->page==UI_CLOSED)regions[count++]=u->ball;
    else regions[count++]=(rect_t){0,0,u->w,u->h};
    if(count==2&&painter_overlap(regions[0],regions[1])){
        regions[0]=painter_union(regions[0],regions[1]);count=1;
    }
    for(int i=0;i<count;i++){
        rect_t r=painter_alpha_bounds(p->canvas,regions[i]);
        if(r.w&&r.h)p->painted[p->painted_count++]=r;
    }
}
void ui_painter_destroy(ui_painter_t *p){painter_glyphs_clear(p);bitmap_free(p->canvas);font_close(p->font);font_close(p->small);memset(p,0,sizeof(*p));}
int ui_painter_render(ui_painter_t *p,quick_ui_t *u,renderer_t *r,const char *fallback_font){
    int changed=0;
    if(!p->canvas||p->canvas->w!=u->w||p->canvas->h!=u->h){bitmap_free(p->canvas);p->canvas=bitmap_alloc(u->w,u->h,BITMAP_FMT_RGBA8888);p->painted_count=0;changed=1;}
    if(!p->canvas)return -1;
    painter_terminal_context(p);
    if(!u->dirty&&!changed)goto draw;
    painter_clear(p);
    if(quick_ui_voice_inline(u)){
        if(ensure_font(p,unit(u,28),fallback_font))return -1;
        voice_inline_text(p,u);voice_capsule(p,u);
    }else if(u->page==UI_CLOSED&&p->complete_ms){
        if(ensure_font(p,unit(u,28),fallback_font))return -1;
        voice_complete(p,u);
    }else if(u->page==UI_CLOSED){
        int alpha=u->ball_pointer?242:u->saved.opacity*255/100;
        round_rect(p->canvas,u->ball,u->saved.edge_handle?unit(u,18):u->ball.h/2,accent,alpha);
        icon(p->canvas,u->ball,u->profile==TERM_PROFILE_TERMINAL?UI_ICON_TERMINAL:u->profile==TERM_PROFILE_CLAUDE?UI_ICON_CLOUD:UI_ICON_KEYBOARD,dark,alpha);
    }else{
        if(ensure_font(p,unit(u,28),fallback_font))return -1;
        if(u->page==UI_QUICK){
            round_rect(p->canvas,(rect_t){0,0,u->w,u->h},0,(rgb_t){0,0,0},70);
            round_rect(p->canvas,(rect_t){unit(u,14),unit(u,72),u->w-unit(u,28),unit(u,630)},unit(u,18),panel,255);
        }else round_rect(p->canvas,(rect_t){0,0,u->w,u->h},0,panel,255);
        painter_text(p,p->canvas,p->font,unit(u,122),unit(u,u->page==UI_QUICK?94:34),u->w-unit(u,140),u->title,ink,panel);
        for(int i=0;i<u->item_count;i++){
            ui_item_t *it=&u->items[i];rgb_t bg=it->selected?accent:button,fg=it->selected?dark:ink;
            if(u->page==UI_VOICE&&!it->action)fg=muted;
            if(i==0)bg=panel;
            round_rect(p->canvas,it->rect,unit(u,12),bg,255);
            if(it->icon!=UI_ICON_NONE)icon(p->canvas,it->rect,it->icon,fg,255);
            else{
                int x=it->rect.x+unit(u,18),y=it->rect.y+unit(u,it->detail[0]?8:18);
                painter_text(p,p->canvas,p->font,x,y,it->rect.w-unit(u,32),it->label,fg,bg);
                if(it->detail[0])painter_text(p,p->canvas,p->small,x,it->rect.y+unit(u,42),it->rect.w-unit(u,32),it->detail,muted,bg);
            }
        }
        if(u->page==UI_QUICK){
            const char *labels[]={"终端","Claude","tty"};
            for(int i=0;i<3;i++)painter_text(p,p->canvas,p->small,unit(u,103+i*228),unit(u,264),unit(u,170),labels[i],muted,panel);
        }
        if(u->page==UI_VOICE)voice_preview(p,u);
        painter_text(p,p->canvas,p->small,unit(u,28),unit(u,u->page==UI_QUICK?660:u->page==UI_VOICE?650:620),u->w-unit(u,56),u->status[0]?u->status:"Esc / Alt → Sym 返回",muted,panel);
        if(!p->cjk)painter_text(p,p->canvas,p->small,unit(u,28),unit(u,658),u->w-unit(u,56),"CJK font missing",ink,panel);
    }
    painter_bounds(p,u);u->dirty=0;
draw:
    for(int i=0;i<p->painted_count;i++){
        bitmap_t view;rect_t b=p->painted[i];
        bitmap_view(&view,p->canvas->pixels+(size_t)b.y*p->canvas->stride+b.x*4,b.w,b.h,p->canvas->stride,BITMAP_FMT_RGBA8888);
        renderer_blend_bitmap(r,b.x,b.y,&view);
    }
    return 0;
}
