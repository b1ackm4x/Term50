#ifndef TERM50_VOICE_AUDIO_H
#define TERM50_VOICE_AUDIO_H

#include <stddef.h>
#include <sys/types.h>

/* A handle belongs to one worker thread; close only after its reads stop.
 * PCM is always interleaved stereo, 48000 Hz, signed 16-bit little endian.
 * No capture or filesystem output occurs until open succeeds. */
typedef struct voice_audio voice_audio_t;

int voice_audio_open(voice_audio_t **out, char *error, size_t cap);
/* Call read once after open to prime SND_PCM_START_DATA capture, then use this
 * borrowed descriptor for read readiness. Drain read() until it returns 0
 * after readiness: one hardware fragment may span several caller buffers. */
int voice_audio_fd(voice_audio_t *audio);
/* Positive: complete 4-byte frames; 0: would block; -1: errno explains the
 * failure. Hard capture failures are latched until close, never auto-retried.
 * The caller buffer must hold at least one frame. */
ssize_t voice_audio_read(voice_audio_t *audio, void *buffer, size_t capacity);
void voice_audio_close(voice_audio_t *audio);

#endif
