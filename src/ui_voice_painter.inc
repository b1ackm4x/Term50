/* Lightweight composition over the terminal, with bounded visual cadence. */
static rgb_t voice_mix(rgb_t a,rgb_t b,int alpha){
    return (rgb_t){(a.r*alpha+b.r*(255-alpha))/255,
        (a.g*alpha+b.g*(255-alpha))/255,(a.b*alpha+b.b*(255-alpha))/255};
}
void ui_painter_voice_anchor(ui_painter_t *p,quick_ui_t *u,font_t *f,
    int x,int y,int cw,int ch,int top,rgb_t bg){
    if(cw<1||ch<1)return;
    if(p->voice_x!=x||p->voice_y!=y||p->cell_w!=cw||p->cell_h!=ch||
       p->grid_top!=top||p->terminal_font!=f||memcmp(&p->terminal_bg,&bg,sizeof(bg)))u->dirty=1;
    p->voice_x=x;p->voice_y=y;p->cell_w=cw;p->cell_h=ch;p->grid_top=top;
    p->terminal_font=f;p->terminal_bg=bg;p->anchor_valid=1;
}
void ui_painter_voice_complete(ui_painter_t *p,uint64_t now){
    p->complete_ms=now?now:1;p->next_frame_ms=0;
}
int ui_painter_voice_tick(ui_painter_t *p,quick_ui_t *u,uint64_t now,unsigned level){
    p->now_ms=now;int visible=quick_ui_voice_inline(u),changed=0;
    if(visible&&!p->inline_visible){
        p->opened_ms=p->state_ms=now;p->next_frame_ms=0;p->complete_ms=0;
        p->previous_text[0]=0;p->last_voice_state=u->voice_state;changed=1;
    }
    if(visible&&p->last_voice_state!=u->voice_state){
        p->last_voice_state=u->voice_state;p->state_ms=now;p->next_frame_ms=0;changed=1;
    }
    if(visible&&strcmp(p->previous_text,u->voice_text)){
        size_t n=0;while(p->previous_text[n]&&p->previous_text[n]==u->voice_text[n])n++;
        while(n&&((unsigned char)u->voice_text[n]&0xc0)==0x80)n--;
        p->changed_from=n;p->text_ms=now;
        memcpy(p->previous_text,u->voice_text,strlen(u->voice_text)+1);changed=1;
    }
    p->inline_visible=visible;
    if(level>1000)level=1000;
    int next=u->voice_state==UI_VOICE_RECORDING?(int)level:0;
    if(p->complete_ms&&now-p->complete_ms>=1200){p->complete_ms=0;changed=1;}
    /* Peak release smoothing is visual only, sampled on animation frames. */
    if((visible||p->complete_ms)&&now>=p->next_frame_ms){
        p->level=next>p->level?next:(p->level*2+next)/3;
        int busy=visible&&u->voice_state>=UI_VOICE_CONNECTING&&u->voice_state<=UI_VOICE_FINALIZING;
        int transition=visible&&(now-p->opened_ms<220||now-p->text_ms<180);
        p->next_frame_ms=now+(busy||transition||p->complete_ms?100:500);
        changed=1;
    }
    if(changed)u->dirty=1;
    return changed;
}
static int voice_cells(UChar32 cp){
    int cat=u_charType(cp);
    if(cat==U_NON_SPACING_MARK||cat==U_ENCLOSING_MARK||cp==0x200d)return 0;
    int width=u_getIntPropertyValue(cp,UCHAR_EAST_ASIAN_WIDTH);
    return width==U_EA_WIDE||width==U_EA_FULLWIDTH?2:1;
}
static int intersects(rect_t a,rect_t b){
    return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;
}
static void voice_inline_text(ui_painter_t *p,quick_ui_t *u){
    if(!p->anchor_valid||!u->voice_text[0]){u->voice_rect=(rect_t){0};return;}
    font_t *f=p->terminal_font?p->terminal_font:p->font;
    int cw=p->cell_w,ch=p->cell_h,left=p->voice_x;
    int right=(u->w/cw)*cw,top=p->voice_y;
    if(left<0)left=0;
    if(left>right)left=right;
    int first_width=right-left,next_width=right;
    size_t len=strlen(u->voice_text),*starts=malloc((len+2)*sizeof(*starts));
    if(!starts)return;
    starts[0]=0;int count=1,x=0,caret_line=0,caret_x=0;
    int32_t i=0;
    while(i<(int32_t)len){
        int32_t start=i;UChar32 cp;U8_NEXT(u->voice_text,i,(int32_t)len,cp);if(cp<0)cp='?';
        int w=cp=='\n'?0:cp=='\t'?cw:voice_cells(cp)*cw;
        int width=count==1?first_width:next_width;
        if(cp!='\n'&&x+w>width&&(x||count==1)){starts[count++]=(size_t)start;x=0;}
        if((size_t)start==u->voice_cursor){caret_line=count-1;caret_x=x;}
        if(cp=='\n'){starts[count++]=(size_t)i;x=0;}else x+=w;
    }
    if(u->voice_cursor==len){caret_line=count-1;caret_x=x;}
    starts[count]=len;
    int available=(u->usable_h-p->grid_top)/ch;
    int visible=count<3?count:3;if(visible>available)visible=available;
    if(visible<1){free(starts);return;}
    if(u->voice_scroll>caret_line)u->voice_scroll=caret_line;
    if(u->voice_scroll+visible<=caret_line)u->voice_scroll=caret_line-visible+1;
    if(u->voice_scroll<0)u->voice_scroll=0;
    if(top+visible*ch>u->usable_h)top=u->usable_h-visible*ch;
    if(top<p->grid_top)top=p->grid_top;
    int box_left=u->voice_scroll||visible>1?0:left;
    u->voice_rect=(rect_t){box_left,top,right-box_left,visible*ch};
    uint64_t age=p->now_ms-p->text_ms;int fade=age>=160?255:150+(int)age*105/160;
    for(int row=u->voice_scroll;row<count&&row<u->voice_scroll+visible;row++){
        int y=top+(row-u->voice_scroll)*ch,origin=row==0?left:0;x=0;
        int32_t off=(int32_t)starts[row],end=(int32_t)starts[row+1];
        /* Only composition cells cover terminal pixels, not the whole row. */
        while(off<end){
            int32_t start=off;UChar32 cp;U8_NEXT(u->voice_text,off,end,cp);if(cp<0)cp='?';if(cp=='\n')break;
            int cells=cp=='\t'?1:voice_cells(cp),w=cells*cw;
            if(w&&origin+x+w<=right){
                rgb_t fg=voice_mix(accent,p->terminal_bg,(size_t)start>=p->changed_from?fade:255);
                const bitmap_t *glyph=painter_glyph(p,f,cp=='\t'?' ':cp,fg,p->terminal_bg,w,ch);
                if(glyph)bitmap_blit(p->canvas,origin+x,y,glyph);
                line(p->canvas,origin+x,y+ch-2,origin+x+w-1,y+ch-2,1,accent,190);
            }
            x+=w;
        }
        if(row==caret_line&&((p->now_ms/500)%2==0||u->voice_state==UI_VOICE_RECORDING)){
            int cx=origin+caret_x;if(cx>right-2)cx=right-2;
            line(p->canvas,cx,y+2,cx,y+ch-4,2,accent,255);
        }
    }
    free(starts);
    rect_t bottom=u->voice_capsule;
    if(intersects(bottom,u->voice_rect)){
        int y=top-bottom.h-unit(u,16);
        if(y<p->grid_top+unit(u,8))y=top+visible*ch+unit(u,16);
        if(y+bottom.h<=u->usable_h-unit(u,8))quick_ui_voice_capsule_position(u,y);
    }
}
static void voice_mic(bitmap_t *b,int cx,int cy,int s,rgb_t c,int alpha){
    round_rect(b,(rect_t){cx-s/4,cy-s/2,s/2,s},s/4,c,alpha);
    line(b,cx-s/2,cy,cx-s/2,cy+s/3,2,c,alpha);
    line(b,cx+s/2,cy,cx+s/2,cy+s/3,2,c,alpha);
    line(b,cx-s/2,cy+s/3,cx+s/2,cy+s/3,2,c,alpha);
    line(b,cx,cy+s/3,cx,cy+s*3/4,2,c,alpha);
}
static void voice_capsule(ui_painter_t *p,quick_ui_t *u){
    rect_t r=u->voice_capsule;uint64_t age=p->now_ms-p->opened_ms;
    int alpha=age>=200?255:100+(int)age*155/200,dy=age>=200?0:(200-(int)age)*6/200;
    r.y+=dy;rgb_t capsule={35,39,46},border={75,83,89};
    round_rect(p->canvas,(rect_t){r.x-1,r.y-1,r.w+2,r.h+2},r.h/2,border,alpha);
    round_rect(p->canvas,r,r.h/2,capsule,alpha);
    int cy=r.y+r.h/2,pad=unit(u,16);
    for(int j=0;j<u->item_count;j++){
        ui_item_t *it=&u->items[j];rect_t item=it->rect;item.y+=dy;
        if(j>0)line(p->canvas,item.x,item.y+unit(u,16),item.x,item.y+item.h-unit(u,16),1,border,alpha);
        int x=item.x+pad,y=item.y+(item.h-font_line_skip(p->small))/2;
        if(it->action&&!strcmp(it->label,"取消")){
            int cx=item.x+item.w/2,s=unit(u,7);
            line(p->canvas,cx-s,cy-s,cx+s,cy+s,2,ink,alpha);
            line(p->canvas,cx-s,cy+s,cx+s,cy-s,2,ink,alpha);continue;
        }
        if(!it->action){
            if(u->voice_state==UI_VOICE_RECORDING){
                voice_mic(p->canvas,x+unit(u,10),cy,unit(u,20),accent,alpha);x+=unit(u,30);
                for(int k=0;k<5;k++){
                    int factor=5-abs(k-2),height=unit(u,4)+(p->level*unit(u,28)*factor)/5000;
                    round_rect(p->canvas,(rect_t){x+k*unit(u,7),cy-height/2,unit(u,3),height},unit(u,1),accent,alpha);
                }
                x+=unit(u,42);char label[64];uint64_t sec=(p->now_ms-p->state_ms)/1000;
                snprintf(label,sizeof(label),"聆听中 %02u:%02u",(unsigned)(sec/60),(unsigned)(sec%60));
                text(p->canvas,p->small,x,y,item.x+item.w-x-unit(u,8),label,voice_mix(ink,capsule,alpha),capsule);
            }else{
                for(int k=0;k<3;k++){
                    int a=(int)((p->now_ms/200)%3)==k?alpha:alpha/3;
                    round_rect(p->canvas,(rect_t){x+k*unit(u,12),cy-unit(u,3),unit(u,6),unit(u,6)},unit(u,3),accent,a);
                }
                x+=unit(u,40);text(p->canvas,p->small,x,y,item.x+item.w-x-unit(u,8),it->label,voice_mix(ink,capsule,alpha),capsule);
            }
        }else{
            if(item.w<unit(u,90)){x=item.x+unit(u,8);pad=unit(u,8);}
            if(j==0&&u->voice_state==UI_VOICE_READY){
                line(p->canvas,x,cy,x+unit(u,6),cy+unit(u,6),2,accent,alpha);
                line(p->canvas,x+unit(u,6),cy+unit(u,6),x+unit(u,18),cy-unit(u,8),2,accent,alpha);
                x+=unit(u,30);
            }
            text(p->canvas,p->small,x,y,item.x+item.w-x-pad,it->label,voice_mix(j==0?accent:ink,capsule,alpha),capsule);
        }
    }
}
static void voice_complete(ui_painter_t *p,quick_ui_t *u){
    int age=(int)(p->now_ms-p->complete_ms),alpha=age<900?255:(1200-age)*255/300;
    if(alpha<0)alpha=0;
    int w=unit(u,220),h=unit(u,56);rect_t r={(u->w-w)/2,u->usable_h-h-unit(u,24),w,h};
    rgb_t bg={35,39,46};round_rect(p->canvas,r,h/2,bg,alpha);
    int x=r.x+unit(u,18),cy=r.y+h/2;
    line(p->canvas,x,cy,x+unit(u,7),cy+unit(u,7),2,accent,alpha);
    line(p->canvas,x+unit(u,7),cy+unit(u,7),x+unit(u,20),cy-unit(u,9),2,accent,alpha);
    text(p->canvas,p->small,x+unit(u,34),r.y+(h-font_line_skip(p->small))/2,w-unit(u,60),"已插入",voice_mix(ink,bg,alpha),bg);
    for(int yy=r.y;yy<r.y+r.h&&yy<p->canvas->h;yy++)for(int xx=r.x;xx<r.x+r.w&&xx<p->canvas->w;xx++){
        unsigned char *a=p->canvas->pixels+(size_t)yy*p->canvas->stride+xx*4+3;
        if(*a==255)*a=(unsigned char)alpha;
    }
}
