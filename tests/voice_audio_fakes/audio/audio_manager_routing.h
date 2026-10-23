#ifndef TEST_VOICE_AUDIO_MANAGER_H
#define TEST_VOICE_AUDIO_MANAGER_H
#include <sys/asoundlib.h>
enum { AUDIO_TYPE_VOICE_RECOGNITION = 2 };
int audio_manager_snd_pcm_open_preferred(int, snd_pcm_t **, unsigned int *,
                                       int *, int *, int);
int audio_manager_free_handle(unsigned int);
#endif
