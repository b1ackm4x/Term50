#ifndef UI_PAINTER_H_
#define UI_PAINTER_H_
#include "quick_ui.h"
#include "renderer.h"
#define UI_PAINTER_GLYPH_CACHE_SIZE 128
typedef struct ui_painter_glyph {
    font_t *font;
    bitmap_t *bitmap;
    uint32_t codepoint,fg,bg,epoch;
    int cell_w,cell_h;
} ui_painter_glyph_t;
typedef struct ui_painter_metric {
    font_t *font;
    uint32_t codepoint,epoch;
    int advance;
} ui_painter_metric_t;
typedef struct ui_painter {
    bitmap_t *canvas;
    /* Disjoint nontransparent overlay regions; unchanged terminal pixels are
     * never scanned or alpha-blended during compact dictation animation. */
    rect_t painted[2];
    int painted_count;
    ui_painter_glyph_t glyphs[UI_PAINTER_GLYPH_CACHE_SIZE];
    ui_painter_metric_t metrics[UI_PAINTER_GLYPH_CACHE_SIZE];
    uint32_t glyph_epoch;
    uint64_t glyph_hits,glyph_renders;
    font_t *cached_terminal_font;
    int cached_cell_w,cached_cell_h;
    font_t *font,*small;
    int font_size, cjk;
    char font_path[1024];
    /* Terminal font is borrowed; animations never own or write PTY text. */
    font_t *terminal_font;
    int voice_x,voice_y,cell_w,cell_h,grid_top,anchor_valid;
    rgb_t terminal_bg;
    uint64_t now_ms,opened_ms,state_ms,text_ms,next_frame_ms,complete_ms;
    int inline_visible,last_voice_state,level;
    size_t changed_from;
    char previous_text[UI_VOICE_TEXT_MAX+1];
} ui_painter_t;
void ui_painter_voice_anchor(ui_painter_t *p,quick_ui_t *u,font_t *font,
    int x,int y,int cell_w,int cell_h,int grid_top,rgb_t bg);
/* At most ten frames/second for active animation; no transcript timer. */
int ui_painter_voice_tick(ui_painter_t *p,quick_ui_t *u,uint64_t now,unsigned level);
void ui_painter_voice_complete(ui_painter_t *p,uint64_t now);
/* Prepare CJK glyph resources before the recording worker begins capture. */
int ui_painter_voice_prepare(ui_painter_t *p,const quick_ui_t *u,const char *fallback_font);
void ui_painter_destroy(ui_painter_t *p);
int ui_painter_render(ui_painter_t *p,quick_ui_t *u,renderer_t *r,const char *fallback_font);
#endif
