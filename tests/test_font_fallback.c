#include "font.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int ink(const bitmap_t *b,int x0,int x1) {
    int n=0;
    for(int y=0;y<b->h;y++)for(int x=x0;x<x1;x++) {
        const unsigned char *p=b->pixels+y*b->stride+x*4;
        if(p[0]||p[1]||p[2])n++;
    }
    return n;
}
int main(int argc,char **argv) {
    assert(argc==3);assert(font_library_init()==0);
    for(int size=12;size<=36;size+=6) {
        font_t *f=font_open(argv[1],size);assert(f);
        int width=0;assert(!font_glyph_metrics(f,'X',0,0,0,0,&width));
        int height=font_line_skip(f);
        rgb_t fg={255,255,255},bg={0,0,0};
        bitmap_t *ascii=font_render_glyph_cell(f,'A',0,fg,bg,width,height);assert(ascii);
        assert(font_glyph_metrics(f,0x4e2d,0,0,0,0,0)==-1);
        assert(font_add_fallback(f,"/no-such-font.ttf")==-1);
        assert(!font_add_fallback(f,argv[2]));
        assert(font_is_fixed_width(f));assert(font_line_skip(f)==height);
        bitmap_t *same=font_render_glyph_cell(f,'A',0,fg,bg,width,height);
        assert(same && same->stride==ascii->stride && same->h==ascii->h);
        assert(!memcmp(same->pixels,ascii->pixels,ascii->h*ascii->stride));
        bitmap_free(same);bitmap_free(ascii);
        const uint32_t chars[]={0x4e2d,0x6587,0x6d4b,0x8bd5,0x7e41,0x9ad4,0xff0c,0x3002};
        for(unsigned i=0;i<sizeof(chars)/sizeof(chars[0]);i++) {
            assert(!font_glyph_metrics(f,chars[i],0,0,0,0,0));
            for(int style=0;style<8;style++) {
                bitmap_t *b=font_render_glyph_cell(f,chars[i],style,fg,bg,2*width,height);
                assert(b&&b->w==2*width&&b->h==height&&ink(b,0,b->w)>0);
                if(i<6)assert(ink(b,0,width)>0&&ink(b,width,2*width)>0);
                bitmap_free(b);
            }
        }
        bitmap_t *missing=font_render_glyph_cell(f,0x10ffff,0,fg,bg,width,height);
        assert(missing);bitmap_free(missing);
        font_close(f);
    }
    font_library_quit();puts("FONT_FALLBACK_ASCII_PRESERVED_CJK_STYLES_AND_CELL_BOUNDS_PASS");
}
