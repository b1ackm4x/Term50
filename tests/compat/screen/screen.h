#ifndef TERM50_TEST_SCREEN_H
#define TERM50_TEST_SCREEN_H
/* Minimal native Screen contract for host tests. Values are test-local. */
typedef struct fake_screen_context *screen_context_t;
typedef struct fake_screen_window *screen_window_t;
typedef struct fake_screen_buffer *screen_buffer_t;
enum {
    SCREEN_PROPERTY_RENDER_BUFFERS=1, SCREEN_PROPERTY_POINTER,
    SCREEN_PROPERTY_STRIDE, SCREEN_PROPERTY_BUFFER_SIZE,
    SCREEN_PROPERTY_SIZE, SCREEN_PROPERTY_RENDER_BUFFER_COUNT
};
int screen_get_window_property_pv(screen_window_t,int,void **);
int screen_get_window_property_iv(screen_window_t,int,int *);
int screen_get_buffer_property_pv(screen_buffer_t,int,void **);
int screen_get_buffer_property_iv(screen_buffer_t,int,int *);
int screen_post_window(screen_window_t,screen_buffer_t,int,const int *,int);
#endif
