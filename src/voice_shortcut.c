#include "voice_shortcut.h"

enum { VOICE_HOLD_MS = 600 };

static int valid(const voice_shortcut_t *shortcut, uint64_t session,
                 uint64_t generation, int eligible)
{
    return eligible && session && shortcut->session == session &&
           shortcut->generation == generation;
}

void voice_shortcut_cancel(voice_shortcut_t *shortcut)
{
    shortcut->pending = 0;
}

int voice_shortcut_tick(voice_shortcut_t *shortcut, uint64_t session,
                        uint64_t generation, int eligible, uint64_t now_ms)
{
    if (!shortcut->pending) return 0;
    if (!valid(shortcut, session, generation, eligible)) {
        shortcut->pending = 0;
        return 0;
    }
    if (now_ms < shortcut->down_ms ||
        now_ms - shortcut->down_ms < VOICE_HOLD_MS)
        return 0;

    shortcut->pending = 0;
    return VOICE_SHORTCUT_TRIGGER;
}

int voice_shortcut_key(voice_shortcut_t *shortcut, const key_event_t *key,
                       uint64_t session, uint64_t generation,
                       int eligible, uint64_t now_ms)
{
    if (shortcut->pending &&
        !valid(shortcut, session, generation, eligible))
        shortcut->pending = 0;

    /* A fresh down recovers from a lost release after focus changes. */
    if (key->sym == '0' && key->pressed && !key->repeat) {
        shortcut->owned = shortcut->pending = 0;
        if (!eligible || !session || key->modifiers) return 0;

        shortcut->owned = shortcut->pending = 1;
        shortcut->session = session;
        shortcut->generation = generation;
        shortcut->down_ms = now_ms;
        shortcut->down = *key;
        return VOICE_SHORTCUT_CONSUME;
    }

    if (shortcut->owned && key->sym == '0') {
        int result = VOICE_SHORTCUT_CONSUME;
        if (!key->pressed) {
            result |= voice_shortcut_tick(shortcut, session, generation,
                                          eligible, now_ms);
            if (shortcut->pending) result |= VOICE_SHORTCUT_REPLAY;
            shortcut->owned = shortcut->pending = 0;
        }
        /* Repeats alone do not define the hold threshold. */
        return result;
    }

    if (shortcut->pending && key->pressed) {
        /* Restore a tapped zero before the next key reaches the terminal. */
        shortcut->pending = 0;
        return VOICE_SHORTCUT_REPLAY;
    }
    return 0;
}
