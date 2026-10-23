#include "voice_commit.h"

#include <errno.h>
#include <string.h>

enum { BRACKET_BYTES = 6 };
static const char bracket_start[] = "\033[200~";
static const char bracket_end[] = "\033[201~";

int voice_commit_prepare(voice_commit_t *commit, const char *text, int bracketed)
{
    size_t length;

    if (!commit || !text) return -1;
    /* The UI accepts at most VOICE_TEXT_MAX bytes. Bound this API too, so a
     * malformed caller cannot make us scan an unbounded string. */
    for (length = 0; length <= VOICE_TEXT_MAX && text[length]; ++length) {}
    if (!length || length > VOICE_TEXT_MAX || !voice_text_valid(text, length))
        return -1;

    memset(commit, 0, sizeof(*commit));
    if (bracketed) {
        memcpy(commit->bytes, bracket_start, BRACKET_BYTES);
        commit->size = BRACKET_BYTES;
    }
    for (size_t i = 0; i < length; ++i) {
        /* Dictation inserts text only. A newline or tab must not execute or
         * alter a shell command before the user reviews it. */
        commit->bytes[commit->size++] =
            text[i] == '\n' || text[i] == '\t' ? ' ' : text[i];
    }
    if (bracketed) {
        memcpy(commit->bytes + commit->size, bracket_end, BRACKET_BYTES);
        commit->size += BRACKET_BYTES;
    }
    return 0;
}

int voice_commit_drain(voice_commit_t *commit,
                       ssize_t (*write_bytes)(void *, const char *, size_t),
                       void *context)
{
    ssize_t written;
    size_t remaining;

    if (!commit || !write_bytes) return -1;
    if (commit->offset >= commit->size) return 1;

    remaining = commit->size - commit->offset;
    written = write_bytes(context, commit->bytes + commit->offset, remaining);
    if (written > 0) {
        if ((size_t)written > remaining) return -1;
        commit->offset += (size_t)written;
        return commit->offset == commit->size;
    }
    if (written < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return -1;
}

size_t voice_commit_abort_tail(const voice_commit_t *commit, char out[16])
{
    size_t offset, end, size = 0;
    int bracketed;

    if (!commit || !commit->offset || commit->offset >= commit->size) return 0;
    offset = commit->offset;
    end = commit->size;
    bracketed = end >= 2 * BRACKET_BYTES &&
        !memcmp(commit->bytes, bracket_start, BRACKET_BYTES);

    if (bracketed) {
        end -= BRACKET_BYTES;
        if (offset < BRACKET_BYTES) {
            size = BRACKET_BYTES - offset;
            memcpy(out, commit->bytes + offset, size);
            offset = BRACKET_BYTES;
        }
        if (offset >= end) {
            memcpy(out, commit->bytes + offset, commit->size - offset);
            return commit->size - offset;
        }
    }

    /* Finish only a code point whose first bytes were already delivered.
     * Replaying complete draft bytes after a partial write is unsafe. */
    while (offset < end &&
           ((unsigned char)commit->bytes[offset] & 0xc0) == 0x80)
        out[size++] = commit->bytes[offset++];
    if (bracketed) {
        memcpy(out + size, bracket_end, BRACKET_BYTES);
        size += BRACKET_BYTES;
    }
    return size;
}
