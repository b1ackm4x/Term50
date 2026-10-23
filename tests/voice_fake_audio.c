/* Host transport fixture: synthetic PCM, never opens a microphone. */
#include "voice_audio.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
struct voice_audio { int open; double start; size_t sent; };
static double fake_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static atomic_uint opened,closed,read_bytes;
unsigned voice_fake_opened(void){return atomic_load(&opened);}
unsigned voice_fake_closed(void){return atomic_load(&closed);}
unsigned voice_fake_bytes(void){return atomic_load(&read_bytes);}
int voice_audio_open(voice_audio_t **out,char *error,size_t cap){
    if(getenv("TERM50_TEST_AUDIO_FAIL_OPEN")){snprintf(error,cap,"synthetic_failure");return -1;}
    *out=calloc(1,sizeof(**out));if(!*out)return -1;
    (*out)->open=1;(*out)->start=fake_now();atomic_fetch_add(&opened,1);
    if(getenv("TERM50_TEST_AUDIO_DELAY_OPEN")){
        struct timespec delay={0,250000000};nanosleep(&delay,NULL);
    }
    return 0;
}
int voice_audio_fd(voice_audio_t *audio){(void)audio;return -1;}
ssize_t voice_audio_read(voice_audio_t *audio,void *buffer,size_t capacity){
    if(!audio||!audio->open)return -1;
    size_t fragment=getenv("TERM50_TEST_AUDIO_BURST")?16384:3840;
    if(getenv("TERM50_TEST_AUDIO_SMALL_FRAGMENTS")){
        size_t available=(size_t)((fake_now()-audio->start)*192000)-audio->sent;
        /* 5 ms converter fragments, a bounded 100 ms driver buffer, and an
         * fd which is not readable for converter-staged data. */
        if(available>19200){errno=EOVERFLOW;return -1;}
        fragment=960;
        if(available<fragment)return 0;
    }
    size_t n=capacity<fragment?capacity:fragment;n-=n%4;
    memset(buffer,0,n);
    const char *sample=getenv("TERM50_TEST_AUDIO_SAMPLE");
    if(sample){
        unsigned value=(unsigned)strtol(sample,NULL,10)&65535u;
        unsigned char *pcm=buffer;
        /* One channel carries a known sample, the other is silent. This
         * catches RMS/averaging mistakes in the promised absolute-peak API. */
        for(size_t i=0;i<n;i+=4){pcm[i]=(unsigned char)value;pcm[i+1]=(unsigned char)(value>>8);}
    }
    audio->sent+=n;atomic_fetch_add(&read_bytes,(unsigned)n);return (ssize_t)n;
}
void voice_audio_close(voice_audio_t *audio){if(audio){audio->open=0;atomic_fetch_add(&closed,1);free(audio);}}
