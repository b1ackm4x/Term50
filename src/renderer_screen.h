#ifndef RENDERER_SCREEN_H_
#define RENDERER_SCREEN_H_

#include "font.h"
#include "platform.h"
#include "renderer.h"

/* Native Screen renderer. Owns the framebuffer view, the symmenu cache, and
 * the glyph cache. The font is borrowed (lifecycle managed by main.c via
 * font_open/font_close); renderer_set_font swaps the borrowed pointer and
 * clears the cache. */
renderer_t *renderer_screen_create(platform_t *platform, font_t *font);

/* Save the terminal before its translucent UI is composed. Animation frames
 * restore the old overlay regions in whichever Screen buffer was latched,
 * refreshing the entire buffer only when its terminal generation changed,
 * avoiding
 * both repeated glyph rasterization and accumulated alpha. Return 0 when
 * unavailable (including after resize); the caller must repaint normally. */
int renderer_screen_capture_base(renderer_t *r);
int renderer_screen_restore_base(renderer_t *r);

#endif /* RENDERER_SCREEN_H_ */
