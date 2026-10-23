/* Explicitly records a bounded interval and reports counts/levels only.
 * Raw audio is neither saved nor transmitted. Device microphone permission
 * is required. Usage: voice-audio-probe --record-ms 1000 */
#define _POSIX_C_SOURCE 200809L
#include "../src/voice_audio.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

static volatile sig_atomic_t interrupted;
static void stop_recording(int signal_number)
{ (void)signal_number; interrupted = 1; }

static long long now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    voice_audio_t *audio = NULL;
    unsigned char buffer[8192];
    char error[256], *end;
    unsigned long long samples = 0, absolute_sum = 0;
    long duration;
    long long start, elapsed;
    int fd, peak = 0, failed = 0;
    if (argc != 3 || strcmp(argv[1], "--record-ms")) {
        fprintf(stderr, "Usage: voice-audio-probe --record-ms 1000\n");
        return 2;
    }
    errno = 0;
    duration = strtol(argv[2], &end, 10);
    if (errno || *end || duration < 100 || duration > 5000) return 2;
    signal(SIGINT, stop_recording);
    signal(SIGTERM, stop_recording);
    if (voice_audio_open(&audio, error, sizeof(error)) < 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    fd = voice_audio_fd(audio);
    start = now_ms();
    if (fd < 0 || fd >= FD_SETSIZE || start < 0) failed = 1;
    while (!failed && !interrupted) {
        fd_set readable;
        struct timeval timeout = { 0, 20000 };
        int ready;
        ssize_t bytes;
        elapsed = now_ms() - start;
        if (elapsed < 0) { failed = 1; break; }
        if (elapsed >= duration) break;
        /* Match the real voice worker: attempt a nonblocking read without
         * requiring a ready descriptor first. SND_PCM_START_DATA devices may
         * need that initial read to start capture. Drain staged data before
         * waiting too, since it need not make the hardware fd readable. */
        do {
            size_t i;
            bytes = voice_audio_read(audio, buffer, sizeof(buffer));
            if (bytes < 0) {
                fprintf(stderr, "Capture failed: %s\n", strerror(errno));
                failed = 1;
                break;
            }
            for (i = 0; i < (size_t)bytes; i += 2) {
                int sample = buffer[i] | ((unsigned int)buffer[i + 1] << 8);
                if (sample >= 32768) sample -= 65536;
                if (sample < 0) sample = -sample;
                if (sample > peak) peak = sample;
                absolute_sum += (unsigned int)sample;
                ++samples;
            }
        } while (bytes > 0 && !interrupted && now_ms() - start < duration);
        if (failed || interrupted) break;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        ready = select(fd + 1, &readable, NULL, NULL, &timeout);
        if (ready < 0 && errno != EINTR) {
            failed = 1;
            break;
        }
    }
    elapsed = now_ms() - start;
    voice_audio_close(audio);
    printf("capture=%s format=48000Hz/stereo/s16le frames=%llu elapsed_ms=%lld peak=%d mean_abs=%llu\n",
           failed ? "error" : interrupted ? "cancelled" : "complete",
           samples / 2, elapsed, peak, samples ? absolute_sum / samples : 0);
    return failed || !samples;
}
