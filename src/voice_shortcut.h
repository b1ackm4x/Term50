#ifndef VOICE_SHORTCUT_H
#define VOICE_SHORTCUT_H

#include <stdint.h>
#include "event.h"

enum {
    VOICE_SHORTCUT_CONSUME = 1,
    VOICE_SHORTCUT_REPLAY = 2,
    VOICE_SHORTCUT_TRIGGER = 4
};
typedef struct {
    int owned, pending;
    uint64_t session, generation, down_ms;
    key_event_t down;
} voice_shortcut_t;

/* A tap is replayed through the existing key pipeline, never written directly
 * to a PTY. An owned long press consumes all repeats and its release. */
int voice_shortcut_key(voice_shortcut_t *, const key_event_t *, uint64_t session,
                       uint64_t generation, int eligible, uint64_t now_ms);
int voice_shortcut_tick(voice_shortcut_t *, uint64_t session,
                        uint64_t generation, int eligible, uint64_t now_ms);
void voice_shortcut_cancel(voice_shortcut_t *);

#endif
