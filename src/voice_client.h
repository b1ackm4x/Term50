#ifndef TERM50_VOICE_CLIENT_H
#define TERM50_VOICE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define VOICE_TEXT_MAX 16384
enum {
    VOICE_IDLE,
    VOICE_CONNECTING,
    VOICE_RECORDING,
    VOICE_FINALIZING,
    VOICE_READY,
    VOICE_ERROR,
    VOICE_INSERTING
};

typedef struct voice_client voice_client_t;
typedef struct {
    int state;
    uint32_t revision;
    unsigned change;
    char text[VOICE_TEXT_MAX + 1];
    char error[129]; /* Stable public error code, never credentials. */
} voice_snapshot_t;

voice_client_t *voice_client_create(void);
/* Numeric address; key is exactly 64 hex characters. No implicit retry. */
int voice_client_start(voice_client_t *, const char *host, int port,
                       const char *key, int probe);
/* Stop asks the peer for a final transcript; cancel discards the exchange.
 * Both requests are asynchronous and safe from the UI thread. */
void voice_client_stop(voice_client_t *);
void voice_client_cancel(voice_client_t *);
/* Copies an internally synchronized snapshot. Rewrites replace the whole text. */
void voice_client_snapshot(voice_client_t *, voice_snapshot_t *);
/* Latest captured S16_LE chunk's absolute peak, 0..1000 of full scale.
 * Thread-safe and independent of transcript change/revision. Silence and any
 * non-recording state (including a stop/cancel request) return zero. */
unsigned voice_client_audio_level(voice_client_t *);
/* Call only after other API callers have stopped using this handle. */
void voice_client_destroy(voice_client_t *);
/* Strict UTF-8 text with no terminal control sequences. LF/TAB are allowed. */
int voice_text_valid(const char *, size_t);

#endif
