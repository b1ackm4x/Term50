#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "voice_client.h"
#include "voice_audio.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum
{
    AUDIO_FRAME_BYTES = 4,
    AUDIO_CHUNK_MAX = 16384,
    WIRE_FRAME_MAX = 65536,
    AUDIO_DRAIN_BATCH = 16
};
typedef enum
{
    REQUEST_CANCEL,
    REQUEST_STOP
} voice_request_t;

struct voice_client
{
    pthread_mutex_t lock;
    pthread_t thread;
    int joinable, running, stop, cancel, probe, port;
    char host[64];
    unsigned char key[32];
    voice_snapshot_t snap;
    unsigned audio_level;
};
static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int requested(voice_client_t *v, voice_request_t request)
{
    int result;
    pthread_mutex_lock(&v->lock);
    result = request == REQUEST_STOP ? v->stop : v->cancel;
    pthread_mutex_unlock(&v->lock);
    return result;
}
static void state(voice_client_t *v, int s, const char *e)
{
    pthread_mutex_lock(&v->lock);
    v->snap.state = s;
    if (s != VOICE_RECORDING)
        v->audio_level = 0;
    snprintf(v->snap.error, sizeof(v->snap.error), "%s", e ? e : "");
    v->snap.change++;
    pthread_mutex_unlock(&v->lock);
}
static void audio_level_update(voice_client_t *v, const unsigned char *pcm, size_t size)
{
    unsigned peak = 0;
    /* The capture contract is interleaved stereo S16_LE. Decode bytes rather
     * than casting: device buffers need not be aligned, and -32768 must be
     * representable before taking the absolute value. No audio is retained. */
    for (size_t i = 0; i + 1 < size; i += 2)
    {
        unsigned sample = (unsigned)pcm[i] | ((unsigned)pcm[i + 1] << 8);
        unsigned magnitude = sample & 0x8000u ? 65536u - sample : sample;
        if (magnitude > peak)
            peak = magnitude;
    }
    unsigned level = (peak * 1000u + 16384u) / 32768u;
    pthread_mutex_lock(&v->lock);
    if (v->snap.state == VOICE_RECORDING && !v->stop && !v->cancel)
        v->audio_level = level;
    pthread_mutex_unlock(&v->lock);
}
int voice_text_valid(const char *s, size_t n)
{
    for (size_t i = 0; i < n;)
    {
        unsigned c = (unsigned char)s[i++], cp = c, extra = 0, min = 0;
        if (c >= 0xc2 && c <= 0xdf)
        {
            cp = c & 31;
            extra = 1;
            min = 0x80;
        }
        else if (c >= 0xe0 && c <= 0xef)
        {
            cp = c & 15;
            extra = 2;
            min = 0x800;
        }
        else if (c >= 0xf0 && c <= 0xf4)
        {
            cp = c & 7;
            extra = 3;
            min = 0x10000;
        }
        else if (c >= 0x80)
            return 0;
        if (i + extra > n)
            return 0;
        for (unsigned j = 0; j < extra; j++)
        {
            c = (unsigned char)s[i++];
            if ((c & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp < 32 && cp != 9 && cp != 10) || (cp >= 0x7f && cp <= 0x9f))
            return 0;
    }
    return 1;
}
static unsigned psk(SSL *ssl, const char *hint, char *id, unsigned idcap, unsigned char *key,
                    unsigned cap)
{
    (void)hint;
    voice_client_t *v = SSL_get_app_data(ssl);
    if (!v || idcap < 7 || cap < 32)
        return 0;
    memcpy(id, "term50", 7);
    memcpy(key, v->key, 32);
    return 32;
}
static int wait_fd(voice_client_t *v, int fd, int wr, double deadline, int stop_too)
{
    while (monotonic_seconds() < deadline && !requested(v, REQUEST_CANCEL) &&
           (!stop_too || !requested(v, REQUEST_STOP)))
    {
        fd_set r, w;
        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_SET(fd, wr ? &w : &r);
        struct timeval tv = {0, 100000};
        int n = select(fd + 1, &r, &w, NULL, &tv);
        if (n > 0)
            return 1;
        if (n < 0 && errno != EINTR)
            return -1;
    }
    return -1;
}
static int connect_socket(voice_client_t *v)
{
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t len;
    struct sockaddr_in *a = (void *)&ss;
    struct sockaddr_in6 *b = (void *)&ss;
    if (inet_pton(AF_INET, v->host, &a->sin_addr) == 1)
    {
        a->sin_family = AF_INET;
        a->sin_port = htons(v->port);
        len = sizeof(*a);
    }
    else if (inet_pton(AF_INET6, v->host, &b->sin6_addr) == 1)
    {
        b->sin6_family = AF_INET6;
        b->sin6_port = htons(v->port);
        len = sizeof(*b);
    }
    else
        return -1;
    int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (fd >= FD_SETSIZE || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    {
        close(fd);
        return -1;
    }
    /* Every captured chunk is one low-latency TLS frame. Do not hold its
     * trailing TCP segment while waiting for acknowledgements of prior audio. */
    int nodelay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
    {
        close(fd);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (connect(fd, (void *)&ss, len) < 0)
    {
        int e = 0;
        socklen_t n = sizeof(e);
        if (errno != EINPROGRESS || wait_fd(v, fd, 1, monotonic_seconds() + 8, 1) < 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n) || e)
        {
            close(fd);
            return -1;
        }
    }
    return fd;
}
static int handshake(voice_client_t *v, SSL *ssl, int fd)
{
    double deadline = monotonic_seconds() + 8;
    for (;;)
    {
        int n = SSL_connect(ssl);
        if (n == 1)
            return 0;
        int e = SSL_get_error(ssl, n);
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) ||
            wait_fd(v, fd, e == SSL_ERROR_WANT_WRITE, deadline, 1) < 0)
            return -1;
    }
}
static int send_frame(voice_client_t *v, SSL *ssl, int fd, char type, const void *data, size_t n)
{
    unsigned char buf[AUDIO_CHUNK_MAX + 5];
    if (n > AUDIO_CHUNK_MAX)
        return -1;
    uint32_t len = htonl((uint32_t)n + 1);
    memcpy(buf, &len, 4);
    buf[4] = (unsigned char)type;
    if (n)
        memcpy(buf + 5, data, n);
    size_t off = 0;
    double deadline = monotonic_seconds() + 3;
    while (off < n + 5)
    {
        if (requested(v, REQUEST_CANCEL) || monotonic_seconds() >= deadline)
            return -1;
        int r = SSL_write(ssl, buf + off, (int)(n + 5 - off));
        if (r > 0)
        {
            off += (size_t)r;
            continue;
        }
        int e = SSL_get_error(ssl, r);
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) ||
            wait_fd(v, fd, e == SSL_ERROR_WANT_WRITE, deadline, type == 'B' || type == 'P') < 0)
            return -1;
    }
    return 0;
}
static int snapshot(voice_client_t *v, const unsigned char *p, size_t n, int final)
{
    if (n < 4 || n - 4 > VOICE_TEXT_MAX || !voice_text_valid((const char *)p + 4, n - 4))
        return -1;
    uint32_t rev;
    memcpy(&rev, p, 4);
    rev = ntohl(rev);
    pthread_mutex_lock(&v->lock);
    if (v->cancel || !rev || rev <= v->snap.revision)
    {
        pthread_mutex_unlock(&v->lock);
        return -1;
    }
    /* A rewrite may be shorter than a partial transcript. Do not expose
     * stale suffix bytes to snapshot callers after the new terminator. */
    size_t prior_length = strlen(v->snap.text);
    size_t text_length = n - 4;
    if (prior_length > text_length)
        memset(v->snap.text + text_length, 0, prior_length - text_length + 1);
    else
        v->snap.text[text_length] = 0;
    memcpy(v->snap.text, p + 4, text_length);
    v->snap.revision = rev;
    if (final)
        v->snap.state = VOICE_READY;
    v->snap.change++;
    pthread_mutex_unlock(&v->lock);
    return 0;
}
static void *worker(void *arg)
{
    voice_client_t *v = arg;
    int fd = -1, done = 0, started = 0, stopped = 0, read_wants_write = 0;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    voice_audio_t *audio = NULL;
    const char *error = "connection_failed";
    unsigned char rx[WIRE_FRAME_MAX + 4];
    size_t used = 0;
    /* OpenSSL 1.0.2 may use write(2): block SIGPIPE only in this worker. */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    fd = connect_socket(v);
    if (fd < 0)
        goto end;
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx)
        goto end;
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION))
        goto end;
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    if (SSL_CTX_set_cipher_list(ctx, "PSK-AES256-GCM-SHA384") != 1)
        goto end;
    SSL_CTX_set_psk_client_callback(ctx, psk);
    ssl = SSL_new(ctx);
    if (!ssl)
        goto end;
    SSL_set_app_data(ssl, v);
    SSL_set_fd(ssl, fd);
    error = "authentication_or_tls_failed";
    if (handshake(v, ssl, fd))
        goto end;
    error = "connection_lost";
    if (send_frame(v, ssl, fd, v->probe ? 'P' : 'B', NULL, 0))
        goto end;
    double deadline = monotonic_seconds() + 30;
    double last_audio = monotonic_seconds(), frame_deadline = 0;
    while (!done && !requested(v, REQUEST_CANCEL))
    {
        if (monotonic_seconds() > deadline)
        {
            error = started && !stopped ? "recording_limit" : "response_timeout";
            break;
        }
        /* A peer must finish a frame, not keep a recording alive by sending
         * an occasional byte of an unfinished transcript. */
        if (used && monotonic_seconds() > frame_deadline)
        {
            error = "response_timeout";
            break;
        }
        if (requested(v, REQUEST_STOP) && !stopped)
        {
            if (!started)
            {
                error = "cancelled";
                break;
            }
            voice_audio_close(audio);
            audio = NULL;
            if (send_frame(v, ssl, fd, 'S', NULL, 0))
                break;
            stopped = 1;
            state(v, VOICE_FINALIZING, NULL);
            deadline = monotonic_seconds() + 45;
        }
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(fd, &rd);
        if (read_wants_write)
            FD_SET(fd, &wr);
        int maxfd = fd, afd = audio ? voice_audio_fd(audio) : -1;
        if (afd >= 0)
        {
            if (afd >= FD_SETSIZE)
            {
                error = "microphone_fd_invalid";
                break;
            }
            FD_SET(afd, &rd);
            if (afd > maxfd)
                maxfd = afd;
        }
        struct timeval tv = {0, 20000};
        int r = SSL_pending(ssl) ? 1 : select(maxfd + 1, &rd, &wr, NULL, &tv);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (requested(v, REQUEST_CANCEL))
            break;
        if (requested(v, REQUEST_STOP) && !started)
        {
            error = "cancelled";
            break;
        }
        if (SSL_pending(ssl) || (r > 0 && (FD_ISSET(fd, &rd) || FD_ISSET(fd, &wr))))
        {
            int n = SSL_read(ssl, rx + used, (int)(sizeof(rx) - used));
            if (n <= 0)
            {
                int e = SSL_get_error(ssl, n);
                if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
                    break;
                read_wants_write = e == SSL_ERROR_WANT_WRITE;
            }
            else
            {
                read_wants_write = 0;
                if (!used)
                    frame_deadline = monotonic_seconds() + 5;
                used += (size_t)n;
            }
            while (used >= 4)
            {
                if (requested(v, REQUEST_CANCEL))
                    goto end;
                uint32_t len;
                memcpy(&len, rx, 4);
                len = ntohl(len);
                if (!len || len > WIRE_FRAME_MAX)
                {
                    error = "protocol_error";
                    goto end;
                }
                if (used < (size_t)len + 4)
                    break;
                char type = (char)rx[4];
                const unsigned char *p = rx + 5;
                size_t size = len - 1;
                if (v->probe && type == 'K' && size == 8 && !memcmp(p, "TERM50/1", 8))
                {
                    state(v, VOICE_READY, NULL);
                    done = 1;
                }
                else if (!v->probe && type == 'R' && !size && !started)
                {
                    char ae[128];
                    error = "microphone_unavailable";
                    if (requested(v, REQUEST_STOP))
                    {
                        error = "cancelled";
                        goto end;
                    }
                    if (voice_audio_open(&audio, ae, sizeof(ae)))
                        goto end;
                    /* Opening a device may yield to the UI thread. A stop or
                     * cancellation during open must never prime capture. */
                    if (requested(v, REQUEST_CANCEL) || requested(v, REQUEST_STOP))
                    {
                        error = "cancelled";
                        goto end;
                    }
                    started = 1;
                    state(v, VOICE_RECORDING, NULL);
                    deadline = monotonic_seconds() + 120;
                    last_audio = monotonic_seconds();
                    error = "connection_lost";
                }
                else if (!v->probe && started && (type == 'T' || (type == 'F' && stopped)))
                {
                    if (snapshot(v, p, size, type == 'F'))
                    {
                        error = "invalid_transcript";
                        goto end;
                    }
                    if (type == 'F')
                        done = 1;
                }
                else if (type == 'E' && size > 0 && size <= 128)
                {
                    char e[129];
                    for (size_t i = 0; i < size; i++)
                        if (p[i] < 33 || p[i] > 126)
                        {
                            error = "protocol_error";
                            goto end;
                        }
                    memcpy(e, p, size);
                    e[size] = 0;
                    state(v, VOICE_ERROR, e);
                    error = NULL;
                    goto end;
                }
                else
                {
                    error = "protocol_error";
                    goto end;
                }
                /* K/F end this exchange. Ignore already-buffered trailing
                 * traffic so it cannot overwrite the accepted final result. */
                if (done)
                    break;
                memmove(rx, rx + 4 + len, used - 4 - len);
                used -= 4 + len;
                frame_deadline = used ? monotonic_seconds() + 5 : 0;
            }
        }
        /* QSA's converter may have several fragments ready without making
         * the underlying descriptor readable. Drain a bounded batch before
         * select; one read per 20 ms can fall behind smaller fragments. Keep
         * checking cancellation and return to network processing each batch. */
        for (int batch = 0; audio && batch < AUDIO_DRAIN_BATCH && !requested(v, REQUEST_CANCEL) &&
                            !requested(v, REQUEST_STOP);
             batch++)
        {
            unsigned char pcm[AUDIO_CHUNK_MAX];
            ssize_t n = voice_audio_read(audio, pcm, sizeof(pcm));
            if (n < 0)
            {
                error = errno == EOVERFLOW ? "microphone_overrun" : "microphone_read_failed";
                goto end;
            }
            if (n)
            {
                last_audio = monotonic_seconds();
                if (n % AUDIO_FRAME_BYTES)
                    goto end;
                audio_level_update(v, pcm, (size_t)n);
                if (send_frame(v, ssl, fd, 'A', pcm, (size_t)n))
                    goto end;
            }
            else
            {
                if (monotonic_seconds() - last_audio > 5)
                {
                    error = "microphone_stalled";
                    goto end;
                }
                break;
            }
            if (monotonic_seconds() > deadline)
            {
                error = "recording_limit";
                goto end;
            }
        }
    }
end:
    voice_audio_close(audio);
    /* Closing TLS also cancels on server. Never replay uncertain audio/commits. */
    if (ssl)
        SSL_free(ssl);
    if (ctx)
        SSL_CTX_free(ctx);
    if (fd >= 0)
        close(fd);
    if (requested(v, REQUEST_CANCEL))
        state(v, VOICE_IDLE, NULL);
    else if (!done && error)
        state(v, VOICE_ERROR, !started && requested(v, REQUEST_STOP) ? "cancelled" : error);
    pthread_mutex_lock(&v->lock);
    v->audio_level = 0;
    OPENSSL_cleanse(v->key, sizeof(v->key));
    v->running = 0;
    pthread_mutex_unlock(&v->lock);
    return NULL;
}
voice_client_t *voice_client_create(void)
{
    voice_client_t *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    if (pthread_mutex_init(&v->lock, NULL))
    {
        free(v);
        return NULL;
    }
    SSL_library_init();
    SSL_load_error_strings();
    return v;
}
static int hex(unsigned char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
int voice_client_start(voice_client_t *v, const char *host, int port, const char *key, int probe)
{
    unsigned char addr[16], decoded[32];
    if (!v || !host || strlen(host) >= sizeof(v->host) || port < 1 || port > 65535 || !key ||
        strlen(key) != 64 ||
        (inet_pton(AF_INET, host, addr) != 1 && inet_pton(AF_INET6, host, addr) != 1))
        return -1;
    for (int i = 0; i < 32; i++)
    {
        int a = hex((unsigned char)key[2 * i]), b = hex((unsigned char)key[2 * i + 1]);
        if (a < 0 || b < 0)
        {
            OPENSSL_cleanse(decoded, 32);
            return -1;
        }
        decoded[i] = (a << 4) | b;
    }
    pthread_mutex_lock(&v->lock);
    if (v->running)
    {
        pthread_mutex_unlock(&v->lock);
        OPENSSL_cleanse(decoded, 32);
        return -1;
    }
    /* running becomes zero only at the worker's final unlock; after that it
     * never accesses v again. Joining it under this lock also serializes two
     * concurrent starts and keeps snapshot/stop from seeing a partial reset. */
    if (v->joinable)
    {
        pthread_join(v->thread, NULL);
        v->joinable = 0;
    }
    strcpy(v->host, host);
    v->port = port;
    memcpy(v->key, decoded, 32);
    OPENSSL_cleanse(decoded, 32);
    v->probe = probe;
    v->stop = v->cancel = 0;
    v->audio_level = 0;
    unsigned change = v->snap.change + 1;
    memset(&v->snap, 0, sizeof(v->snap));
    v->snap.change = change;
    v->snap.state = VOICE_CONNECTING;
    v->running = 1;
    /* QNX's small platform default must hold both bounded frame buffers and
     * the TLS call stack. One worker has a fixed, explicit memory budget. */
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (!rc)
    {
        rc = pthread_attr_setstacksize(&attr, 512 * 1024);
        if (!rc)
            rc = pthread_create(&v->thread, &attr, worker, v);
        pthread_attr_destroy(&attr);
    }
    if (rc)
    {
        v->running = 0;
        OPENSSL_cleanse(v->key, 32);
        v->snap.state = VOICE_ERROR;
        strcpy(v->snap.error, "worker_failed");
        v->snap.change++;
        pthread_mutex_unlock(&v->lock);
        return -1;
    }
    v->joinable = 1;
    pthread_mutex_unlock(&v->lock);
    return 0;
}
void voice_client_stop(voice_client_t *v)
{
    if (v)
    {
        pthread_mutex_lock(&v->lock);
        v->stop = 1;
        v->audio_level = 0;
        pthread_mutex_unlock(&v->lock);
    }
}
void voice_client_cancel(voice_client_t *v)
{
    if (v)
    {
        pthread_mutex_lock(&v->lock);
        v->cancel = 1;
        v->audio_level = 0;
        pthread_mutex_unlock(&v->lock);
    }
}
void voice_client_snapshot(voice_client_t *v, voice_snapshot_t *s)
{
    if (v && s)
    {
        pthread_mutex_lock(&v->lock);
        *s = v->snap;
        pthread_mutex_unlock(&v->lock);
    }
}
unsigned voice_client_audio_level(voice_client_t *v)
{
    if (!v)
        return 0;
    pthread_mutex_lock(&v->lock);
    unsigned level =
        v->snap.state == VOICE_RECORDING && !v->stop && !v->cancel ? v->audio_level : 0;
    pthread_mutex_unlock(&v->lock);
    return level;
}
void voice_client_destroy(voice_client_t *v)
{
    if (!v)
        return;
    voice_client_cancel(v);
    if (v->joinable)
        pthread_join(v->thread, NULL);
    pthread_mutex_destroy(&v->lock);
    OPENSSL_cleanse(v, sizeof(*v));
    free(v);
}
