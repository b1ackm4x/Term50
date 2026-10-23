/* Exercise the real QNX branch with a deterministic fake device. Actual SDK
 * declarations are checked separately by the native cross compilation. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/asoundlib.h>
#include <audio/audio_manager_routing.h>
#include "../src/voice_audio.h"

static snd_pcm_t pcm;
static snd_pcm_channel_params_t requested;
static int status_value, overrun_count, next_read, read_calls;
static int bad_format, open_error, prepare_error, huge_fragment;
static int flushed, closed, freed, capture_stopped, empty_read_status;
static unsigned int disabled_plugins;

static void reset(void)
{
    memset(&requested, 0, sizeof(requested));
    status_value = SND_PCM_STATUS_PREPARED;
    overrun_count = read_calls = bad_format = open_error = prepare_error = 0;
    huge_fragment = flushed = closed = freed = capture_stopped = 0;
    empty_read_status = 0;
    disabled_plugins = 0;
    next_read = 16;
}

int audio_manager_snd_pcm_open_preferred(int type, snd_pcm_t **out,
        unsigned int *manager, int *card, int *device, int mode)
{
    assert(type == AUDIO_TYPE_VOICE_RECOGNITION);
    assert(mode == (SND_PCM_OPEN_CAPTURE | SND_PCM_OPEN_NONBLOCK));
    if (open_error) return -open_error;
    *out = &pcm; *manager = 123; *card = 0; *device = 0;
    return 0;
}
int snd_pcm_plugin_info(snd_pcm_t *p, snd_pcm_channel_info_t *info)
{
    assert(p == &pcm && info->channel == SND_PCM_CHANNEL_CAPTURE);
    info->min_fragment_size = huge_fragment ? 131072 : 16;
    info->max_fragment_size = huge_fragment ? 262144 : 65536;
    info->fragment_align = 4;
    return 0;
}
unsigned int snd_pcm_plugin_set_disable(snd_pcm_t *p, unsigned int mask)
{
    assert(p == &pcm && requested.format.rate == 0);
    disabled_plugins = mask;
    return mask;
}
int snd_pcm_plugin_params(snd_pcm_t *p, snd_pcm_channel_params_t *params)
{
    assert(p == &pcm);
    requested = *params;
    assert(params->format.rate == 48000 && params->format.voices == 2);
    assert(params->format.interleave && params->format.format == SND_PCM_SFMT_S16_LE);
    assert(params->buf.block.frag_size == 3840);
    assert(params->buf.block.frags_max == 32);
    assert(params->stop_mode == SND_PCM_STOP_STOP);
    return 0;
}
int snd_pcm_plugin_setup(snd_pcm_t *p, snd_pcm_channel_setup_t *setup)
{
    assert(p == &pcm && setup->channel == SND_PCM_CHANNEL_CAPTURE);
    *setup = requested;
    /* Hardware may negotiate a fragment different from the requested size. */
    setup->buf.block.frag_size = 16;
    setup->buf.block.frags = 8;
    if (bad_format) setup->format.rate = 44100;
    return 0;
}
int snd_pcm_nonblock_mode(snd_pcm_t *p, int enabled)
{ assert(p == &pcm && enabled == 1); return 0; }
int snd_pcm_file_descriptor(snd_pcm_t *p, int channel)
{ assert(p == &pcm && channel == SND_PCM_CHANNEL_CAPTURE); return 7; }
int snd_pcm_plugin_prepare(snd_pcm_t *p, int channel)
{ assert(p == &pcm && channel == SND_PCM_CHANNEL_CAPTURE); return -prepare_error; }
int snd_pcm_plugin_status(snd_pcm_t *p, snd_pcm_channel_status_t *status)
{
    assert(p == &pcm && status->channel == SND_PCM_CHANNEL_CAPTURE);
    status->status = status_value; status->overrun = overrun_count;
    return 0;
}
ssize_t snd_pcm_plugin_read(snd_pcm_t *p, void *buffer, size_t bytes)
{
    size_t i;
    assert(p == &pcm && bytes == 16);
    ++read_calls;
    /* Regression fixture for the Q20 mmap path: it overruns before reliable
     * capture. The production choice must leave this path before setup. */
    if (!(disabled_plugins & PLUGIN_DISABLE_MMAP)) {
        status_value = SND_PCM_STATUS_OVERRUN;
        return -EIO;
    }
    if (capture_stopped) { status_value = SND_PCM_STATUS_OVERRUN; return -EIO; }
    if (empty_read_status) { status_value = empty_read_status; return 0; }
    if (next_read < 0) return next_read;
    for (i = 0; i < (size_t)next_read; ++i) ((unsigned char *)buffer)[i] = (unsigned char)i;
    return next_read;
}
int snd_pcm_plugin_flush(snd_pcm_t *p, int channel)
{ assert(p == &pcm && channel == SND_PCM_CHANNEL_CAPTURE); ++flushed; return 0; }
int snd_pcm_close(snd_pcm_t *p)
{ assert(p == &pcm); ++closed; return 0; }
int audio_manager_free_handle(unsigned int manager)
{ assert(manager == 123 && closed == 1); ++freed; return 0; }

int main(void)
{
    voice_audio_t *audio;
    unsigned char bytes[32];
    char error[100];
    int i;
    reset();
    assert(voice_audio_open(&audio, error, sizeof(error)) == 0);
    assert(error[0] == 0 && voice_audio_fd(audio) == 7);
    assert(voice_audio_read(audio, bytes, 7) == 4);
    assert(read_calls == 1);
    for (i = 0; i < 4; ++i) assert(bytes[i] == i);
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == 12);
    assert(read_calls == 1);
    for (i = 0; i < 12; ++i) assert(bytes[i] == i + 4);
    next_read = -EAGAIN;
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == 0);
    next_read = -EINTR;
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == 0);
    next_read = 16;
    assert(voice_audio_read(audio, bytes, 3) == -1 && errno == EINVAL);
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == 16);
    voice_audio_close(audio);
    assert(flushed == 1 && closed == 1 && freed == 1);

    reset();
    assert(voice_audio_open(&audio, error, sizeof(error)) == 0);
    overrun_count = 1;
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == -1 && errno == EOVERFLOW);
    overrun_count = 0;
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == -1 && errno == EOVERFLOW);
    assert(read_calls == 0 && voice_audio_fd(audio) == -1);
    voice_audio_close(audio);

    for (i = 0; i < 2; ++i) {
        reset();
        assert(voice_audio_open(&audio, error, sizeof(error)) == 0);
        empty_read_status = i ? SND_PCM_STATUS_PREEMPTED : SND_PCM_STATUS_OVERRUN;
        assert(voice_audio_read(audio, bytes, sizeof(bytes)) == -1);
        assert(errno == (i ? EBUSY : EOVERFLOW));
        assert(read_calls == 1 && voice_audio_fd(audio) == -1);
        voice_audio_close(audio);
        assert(flushed == 1 && closed == 1 && freed == 1);
    }

    reset();
    assert(voice_audio_open(&audio, error, sizeof(error)) == 0);
    capture_stopped = 1;
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == -1 && errno == EOVERFLOW);
    voice_audio_close(audio);

    reset();
    assert(voice_audio_open(&audio, error, sizeof(error)) == 0);
    status_value = SND_PCM_STATUS_PREEMPTED;
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == -1 && errno == EBUSY);
    voice_audio_close(audio);

    reset();
    assert(voice_audio_open(&audio, error, sizeof(error)) == 0);
    next_read = 3;
    assert(voice_audio_read(audio, bytes, sizeof(bytes)) == -1 && errno == EIO);
    voice_audio_close(audio);

    reset(); bad_format = 1;
    assert(voice_audio_open(&audio, error, sizeof(error)) == -1 && audio == NULL);
    assert(errno == ENOTSUP && error[0] && flushed == 1 && closed == 1 && freed == 1);
    reset(); huge_fragment = 1;
    assert(voice_audio_open(&audio, error, sizeof(error)) == -1 && audio == NULL);
    assert(errno == ENOTSUP && flushed == 0 && closed == 1 && freed == 1);
    reset(); prepare_error = EACCES;
    assert(voice_audio_open(&audio, error, sizeof(error)) == -1 && audio == NULL);
    assert(errno == EACCES && flushed == 1 && closed == 1 && freed == 1);
    reset(); open_error = EACCES;
    assert(voice_audio_open(&audio, error, sizeof(error)) == -1 && audio == NULL);
    assert(errno == EACCES && closed == 0 && freed == 0);
    assert(voice_audio_open(NULL, error, sizeof(error)) == -1 && errno == EINVAL);
    voice_audio_close(NULL);
    puts("voice audio: fragment framing, would-block, overrun, preemption, setup validation, cleanup PASS");
    return 0;
}
