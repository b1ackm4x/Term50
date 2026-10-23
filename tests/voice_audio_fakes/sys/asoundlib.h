#ifndef TEST_VOICE_ASOUNDLIB_H
#define TEST_VOICE_ASOUNDLIB_H
#include <sys/types.h>
#include <stddef.h>
typedef struct snd_pcm { int unused; } snd_pcm_t;
enum {
    SND_PCM_CHANNEL_CAPTURE = 1, SND_PCM_MODE_BLOCK = 1,
    SND_PCM_START_DATA = 1, SND_PCM_STOP_STOP = 1,
    SND_PCM_SFMT_S16_LE = 4, SND_PCM_OPEN_CAPTURE = 2,
    SND_PCM_OPEN_NONBLOCK = 0x1000,
    SND_PCM_STATUS_READY = 1, SND_PCM_STATUS_PREPARED = 2,
    SND_PCM_STATUS_RUNNING = 3, SND_PCM_STATUS_OVERRUN = 5,
    SND_PCM_STATUS_UNSECURE = 6, SND_PCM_STATUS_PREEMPTED = 10002
};
#define PLUGIN_DISABLE_MMAP (1u << 1)
struct test_format { int interleave, format, rate, voices; };
struct test_block { int frag_size, frags_min, frags_max, frags; };
typedef struct { int channel, min_fragment_size, max_fragment_size, fragment_align; }
    snd_pcm_channel_info_t;
typedef struct {
    int channel, mode, start_mode, stop_mode;
    struct { struct test_block block; } buf;
    struct test_format format;
} snd_pcm_channel_params_t;
typedef snd_pcm_channel_params_t snd_pcm_channel_setup_t;
typedef struct { int channel, status, overrun; } snd_pcm_channel_status_t;
int snd_pcm_plugin_info(snd_pcm_t *, snd_pcm_channel_info_t *);
unsigned int snd_pcm_plugin_set_disable(snd_pcm_t *, unsigned int);
int snd_pcm_plugin_params(snd_pcm_t *, snd_pcm_channel_params_t *);
int snd_pcm_plugin_setup(snd_pcm_t *, snd_pcm_channel_setup_t *);
int snd_pcm_nonblock_mode(snd_pcm_t *, int);
int snd_pcm_file_descriptor(snd_pcm_t *, int);
int snd_pcm_plugin_prepare(snd_pcm_t *, int);
int snd_pcm_plugin_status(snd_pcm_t *, snd_pcm_channel_status_t *);
ssize_t snd_pcm_plugin_read(snd_pcm_t *, void *, size_t);
int snd_pcm_plugin_flush(snd_pcm_t *, int);
int snd_pcm_close(snd_pcm_t *);
#endif
