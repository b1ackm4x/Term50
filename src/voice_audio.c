#include "voice_audio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __QNXNTO__
#include <sys/asoundlib.h>
#include <audio/audio_manager_routing.h>

enum {
    FRAME_BYTES = 4,
    REQUEST_FRAGMENT_BYTES = 3840, /* 20 ms at 48 kHz stereo. */
    MAX_FRAGMENT_BYTES = 65536,
    MAX_DRIVER_BUFFER_BYTES = 1048576
};

struct voice_audio {
    snd_pcm_t *pcm;
    unsigned int manager;
    int manager_owned;
    int configured;
    int fd;
    int failed;
    unsigned char *fragment;
    size_t fragment_size;
    size_t used;
    size_t offset;
};

/* QSA returns negative error numbers. Some implementations also use the
 * POSIX -1/errno convention; callers clear errno immediately before a call. */
static int pcm_error(int result)
{
    if (result == -1 && errno) return errno;
    return result < 0 ? -result : EIO;
}

static int fail_read(voice_audio_t *audio, int error)
{
    audio->failed = error;
    audio->used = audio->offset = 0;
    errno = error;
    return -1;
}

static int capture_status(voice_audio_t *audio)
{
    snd_pcm_channel_status_t status;
    int rc;
    memset(&status, 0, sizeof(status));
    status.channel = SND_PCM_CHANNEL_CAPTURE;
    errno = 0;
    rc = snd_pcm_plugin_status(audio->pcm, &status);
    if (rc < 0) return fail_read(audio, pcm_error(rc));
    /* Check the counter as well: do not accept audio with an unnoticed gap. */
    if (status.overrun || status.status == SND_PCM_STATUS_OVERRUN)
        return fail_read(audio, EOVERFLOW);
    if (status.status == SND_PCM_STATUS_PREEMPTED)
        return fail_read(audio, EBUSY);
    if (status.status == SND_PCM_STATUS_UNSECURE)
        return fail_read(audio, EACCES);
    if (status.status != SND_PCM_STATUS_PREPARED &&
        status.status != SND_PCM_STATUS_RUNNING)
        return fail_read(audio, EIO);
    return 0;
}

void voice_audio_close(voice_audio_t *audio)
{
    int saved_errno = errno;
    if (!audio) return;
    if (audio->pcm) {
        /* Flush stops capture and discards unread audio; no recording survives
         * cancellation. The PCM handle must close before the manager handle. */
        if (audio->configured)
            snd_pcm_plugin_flush(audio->pcm, SND_PCM_CHANNEL_CAPTURE);
        snd_pcm_close(audio->pcm);
    }
    if (audio->manager_owned) audio_manager_free_handle(audio->manager);
    if (audio->fragment) {
        /* Avoid leaving a reusable heap buffer containing the last fragment. */
        volatile unsigned char *p = audio->fragment;
        size_t i;
        for (i = 0; i < audio->fragment_size; ++i) p[i] = 0;
        free(audio->fragment);
    }
    free(audio);
    errno = saved_errno;
}

int voice_audio_open(voice_audio_t **out, char *error, size_t cap)
{
    voice_audio_t *audio = NULL;
    snd_pcm_channel_info_t info;
    snd_pcm_channel_params_t params;
    snd_pcm_channel_setup_t setup;
    const char *stage = "allocate microphone";
    int rc, failure = ENOMEM, card, device, alignment, frag;
    if (error && cap) error[0] = '\0';
    if (!out) {
        errno = EINVAL;
        if (error && cap) snprintf(error, cap, "Invalid microphone output pointer");
        return -1;
    }
    *out = NULL;
    audio = calloc(1, sizeof(*audio));
    if (!audio) goto failed;
    audio->fd = -1;
    stage = "open microphone (check microphone permission)";
    errno = 0;
    rc = audio_manager_snd_pcm_open_preferred(AUDIO_TYPE_VOICE_RECOGNITION,
            &audio->pcm, &audio->manager, &card, &device,
            SND_PCM_OPEN_CAPTURE | SND_PCM_OPEN_NONBLOCK);
    if (rc < 0) { failure = pcm_error(rc); goto failed; }
    audio->manager_owned = 1;
    if (!audio->pcm) { failure = EIO; goto failed; }

    /* Q20's default mmap plugin cannot sustain this nonblocking read/status
     * loop: capture enters OVERRUN within the first few fragments. Use QSA's
     * message-based path before querying/configuring the plugin chain. This
     * retains format conversion, bounded reads and explicit overrun checks. */
    snd_pcm_plugin_set_disable(audio->pcm, PLUGIN_DISABLE_MMAP);

    stage = "query microphone capabilities";
    memset(&info, 0, sizeof(info));
    info.channel = SND_PCM_CHANNEL_CAPTURE;
    errno = 0;
    rc = snd_pcm_plugin_info(audio->pcm, &info);
    if (rc < 0) { failure = pcm_error(rc); goto failed; }
    if (info.min_fragment_size <= 0 ||
        info.max_fragment_size < info.min_fragment_size ||
        info.min_fragment_size > MAX_FRAGMENT_BYTES ||
        info.fragment_align < 0 || info.fragment_align > MAX_FRAGMENT_BYTES) {
        failure = ENOTSUP;
        goto failed;
    }
    /* Account for driver alignment as well as whole interleaved frames. */
    alignment = info.fragment_align ? info.fragment_align : FRAME_BYTES;
    while (alignment % FRAME_BYTES) {
        if (alignment > MAX_FRAGMENT_BYTES / 2) {
            failure = ENOTSUP;
            goto failed;
        }
        alignment *= 2;
    }
    frag = REQUEST_FRAGMENT_BYTES;
    if (frag < info.min_fragment_size) frag = info.min_fragment_size;
    frag = ((frag + alignment - 1) / alignment) * alignment;
    if (frag > info.max_fragment_size)
        frag = (info.max_fragment_size / alignment) * alignment;
    if (frag < info.min_fragment_size || frag > MAX_FRAGMENT_BYTES) {
        failure = ENOTSUP;
        goto failed;
    }

    stage = "configure 48000 Hz stereo microphone";
    memset(&params, 0, sizeof(params));
    params.channel = SND_PCM_CHANNEL_CAPTURE;
    params.mode = SND_PCM_MODE_BLOCK;
    params.start_mode = SND_PCM_START_DATA;
    /* Rollover would silently discard speech under network backpressure. */
    params.stop_mode = SND_PCM_STOP_STOP;
    params.buf.block.frag_size = frag;
    params.buf.block.frags_min = 1;
    /* Allow roughly 0.7 s of scheduling headroom on Q20. A full-screen
     * Chinese UI repaint can exceed the old ~190 ms ring at recording start.
     * Reads still begin immediately; this is capacity, not a batching delay.
     * Keep STOP semantics so exhausting it remains an explicit failure. */
    params.buf.block.frags_max = 32;
    params.format.interleave = 1;
    params.format.format = SND_PCM_SFMT_S16_LE;
    params.format.rate = 48000;
    params.format.voices = 2;
    errno = 0;
    rc = snd_pcm_plugin_params(audio->pcm, &params);
    if (rc < 0) { failure = pcm_error(rc); goto failed; }
    audio->configured = 1;

    stage = "validate microphone format";
    memset(&setup, 0, sizeof(setup));
    setup.channel = SND_PCM_CHANNEL_CAPTURE;
    errno = 0;
    rc = snd_pcm_plugin_setup(audio->pcm, &setup);
    if (rc < 0) { failure = pcm_error(rc); goto failed; }
    if (setup.mode != SND_PCM_MODE_BLOCK || !setup.format.interleave ||
        setup.format.format != SND_PCM_SFMT_S16_LE ||
        setup.format.rate != 48000 || setup.format.voices != 2 ||
        setup.buf.block.frag_size <= 0 ||
        setup.buf.block.frag_size > MAX_FRAGMENT_BYTES ||
        setup.buf.block.frag_size % FRAME_BYTES ||
        setup.buf.block.frags <= 0 ||
        setup.buf.block.frags > MAX_DRIVER_BUFFER_BYTES / setup.buf.block.frag_size) {
        failure = ENOTSUP;
        goto failed;
    }
    audio->fragment_size = (size_t)setup.buf.block.frag_size;
    audio->fragment = malloc(audio->fragment_size);
    if (!audio->fragment) { failure = ENOMEM; goto failed; }

    stage = "set nonblocking microphone";
    errno = 0;
    rc = snd_pcm_nonblock_mode(audio->pcm, 1);
    if (rc < 0) { failure = pcm_error(rc); goto failed; }
    errno = 0;
    audio->fd = snd_pcm_file_descriptor(audio->pcm, SND_PCM_CHANNEL_CAPTURE);
    if (audio->fd < 0) { failure = pcm_error(audio->fd); goto failed; }
    stage = "prepare microphone";
    errno = 0;
    rc = snd_pcm_plugin_prepare(audio->pcm, SND_PCM_CHANNEL_CAPTURE);
    if (rc < 0) { failure = pcm_error(rc); goto failed; }
    *out = audio;
    return 0;

failed:
    voice_audio_close(audio);
    if (error && cap) snprintf(error, cap, "Cannot %s: %s", stage, strerror(failure));
    errno = failure;
    return -1;
}

int voice_audio_fd(voice_audio_t *audio)
{
    if (!audio) { errno = EINVAL; return -1; }
    if (audio->failed) { errno = audio->failed; return -1; }
    return audio->fd;
}

ssize_t voice_audio_read(voice_audio_t *audio, void *buffer, size_t capacity)
{
    ssize_t count;
    size_t take;
    int failure;
    if (!audio || !buffer || capacity < FRAME_BYTES) {
        errno = EINVAL;
        return -1;
    }
    if (audio->failed) { errno = audio->failed; return -1; }
    if (capture_status(audio) < 0) return -1;
    if (audio->offset == audio->used) {
        errno = 0;
        count = snd_pcm_plugin_read(audio->pcm, audio->fragment,
                                    audio->fragment_size);
        if (count < 0) {
            failure = pcm_error((int)count);
            /* A read can detect an overrun that happened after the preceding
             * status query. Preserve that cause, including on EAGAIN paths. */
            if (capture_status(audio) < 0) return -1;
            if (failure == EAGAIN || failure == EWOULDBLOCK || failure == EINTR)
                return 0;
            return fail_read(audio, failure);
        }
        if (!count) return capture_status(audio) < 0 ? -1 : 0;
        if ((size_t)count > audio->fragment_size || count % FRAME_BYTES)
            return fail_read(audio, EIO);
        if (capture_status(audio) < 0) return -1;
        audio->offset = 0;
        audio->used = (size_t)count;
    }
    take = capacity - capacity % FRAME_BYTES;
    if (take > audio->used - audio->offset) take = audio->used - audio->offset;
    memcpy(buffer, audio->fragment + audio->offset, take);
    audio->offset += take;
    return (ssize_t)take;
}

#else

int voice_audio_open(voice_audio_t **out, char *error, size_t cap)
{
    if (out) *out = NULL;
    if (error && cap)
        snprintf(error, cap, "Native microphone capture requires BlackBerry 10");
    errno = out ? ENOSYS : EINVAL;
    return -1;
}

int voice_audio_fd(voice_audio_t *audio)
{
    (void)audio;
    errno = ENOSYS;
    return -1;
}

ssize_t voice_audio_read(voice_audio_t *audio, void *buffer, size_t capacity)
{
    (void)audio;
    (void)buffer;
    (void)capacity;
    errno = ENOSYS;
    return -1;
}

void voice_audio_close(voice_audio_t *audio) { (void)audio; }

#endif
