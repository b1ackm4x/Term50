#ifndef TERM50_VOICE_COMMIT_H
#define TERM50_VOICE_COMMIT_H

#include <stddef.h>
#include <sys/types.h>
#include "voice_client.h"

typedef struct {
    char bytes[VOICE_TEXT_MAX + 13];
    size_t size, offset;
} voice_commit_t;

/* Always converts line breaks/tabs to spaces: never submits a command. */
int voice_commit_prepare(voice_commit_t *, const char *, int bracketed);
/* 1 done, 0 would-block/partial, -1 permanent failure. Never retries old bytes. */
int voice_commit_drain(voice_commit_t *,
                       ssize_t (*write_bytes)(void *, const char *, size_t),
                       void *);
/* Minimal UTF-8 / bracket suffix after aborted partial delivery; capacity 16. */
size_t voice_commit_abort_tail(const voice_commit_t *, char out[16]);

#endif
