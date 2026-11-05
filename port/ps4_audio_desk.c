/* WinePS4: the desktop's mixer, where programs' sound is played.
 *
 * Each audio stream of a program (wineps4.drv's audio.c) comes over PS4_AUDIO_SOCKET with its
 * ring of frames in shared memory (port/include/ps4_audio.h). Every 256 frames of the port (5.3 ms)
 * the streams that run are converted to 48 kHz stereo (their speakers folded into two, their rate
 * by linear interpolation), summed and played through one sceAudioOut port of the desktop's: one
 * port for all programs, in the app's main process, which the PS4 lets have one (processes started
 * outside the PS4's 7 may not), like PulseAudio for Linux's Wine. A stream's socket closing (its
 * program ended) drops it.
 *
 * Built with ps4_wine_compat.h: the kernel's (FreeBSD) msghdr, cmsghdr and sockaddr_un. */
#include "ps4_wine_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "ps4_audio.h"

int32_t sceAudioOutInit( void );
int32_t sceAudioOutOpen( int32_t user, int32_t type, int32_t index, uint32_t frames, uint32_t rate, uint32_t param );
int32_t sceAudioOutOutput( int32_t port, const void *buffer );
int32_t sceAudioOutSetVolume( int32_t port, int32_t flag, int32_t *volume );

#define GRAIN        256    /* frames the port takes at a time */
#define MAX_STREAMS  32
#define USER_SYSTEM  0xff   /* any user hears the main port */
#define PORT_MAIN    0
#define S16_STEREO   1
#define MAX_RING     (64u << 20)

struct stream
{
    int sock;
    struct ps4_audio_ring *ring;
    size_t size;
    unsigned int id;
    /* the ring's format, as it was when the stream came (the program can't change it under the mixer) */
    uint32_t format, rate, channels, channel_mask, frame_bytes, ring_frames;
    unsigned int sample_bytes;
    char name[64];
    float weight[PS4_AUDIO_CHANNELS][2];  /* each channel's share of left and right */
    double frac;                          /* the resampler's place between prev and next */
    float prev[2], next[2];
    int had_frames;                       /* (an underrun is counted when it had some) */
    uint32_t logged_underruns;
};

static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stream streams[MAX_STREAMS];
static int stream_count;
static unsigned int next_id;
static int audio_log = 1;  /* Settings > Diagnostics > Audio log (WINEPS4_AUDIO_LOG=0: off) */
static pthread_once_t mixer_once = PTHREAD_ONCE_INIT;

static double now_seconds( void )
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* WAVEFORMATEXTENSIBLE's speakers, in their bits' order: where each goes in stereo */
static void speaker_weights( struct stream *s )
{
    static const struct { uint32_t speaker; float left, right; } speakers[] =
    {
        { 0x001, 1.0f, 0.0f },    /* front left */
        { 0x002, 0.0f, 1.0f },    /* front right */
        { 0x004, 0.707f, 0.707f },/* front center */
        { 0x008, 0.35f, 0.35f },  /* low frequency */
        { 0x010, 0.707f, 0.0f },  /* back left */
        { 0x020, 0.0f, 0.707f },  /* back right */
        { 0x040, 0.9f, 0.3f },    /* front left of center */
        { 0x080, 0.3f, 0.9f },    /* front right of center */
        { 0x100, 0.5f, 0.5f },    /* back center */
        { 0x200, 0.707f, 0.0f },  /* side left */
        { 0x400, 0.0f, 0.707f },  /* side right */
    };
    uint32_t mask = s->channel_mask, c, bit = 0, k;
    float left = 0, right = 0, scale;

    if (s->channels == 1)  /* mono: both sides */
    {
        s->weight[0][0] = s->weight[0][1] = 1.0f;
        return;
    }
    if (s->channels == 2 && (!mask || mask == 0x3))
    {
        s->weight[0][0] = s->weight[1][1] = 1.0f;
        s->weight[0][1] = s->weight[1][0] = 0.0f;
        return;
    }
    for (c = 0; c < s->channels; c++)
    {
        uint32_t speaker = 0;

        while (bit < 32 && !(mask & (1u << bit))) bit++;
        if (bit < 32) speaker = 1u << bit++;
        s->weight[c][0] = s->weight[c][1] = 0.5f;  /* (the top ones, and channels past the mask) */
        for (k = 0; k < sizeof(speakers) / sizeof(speakers[0]); k++)
            if (speakers[k].speaker == speaker)
            {
                s->weight[c][0] = speakers[k].left;
                s->weight[c][1] = speakers[k].right;
            }
        left += s->weight[c][0];
        right += s->weight[c][1];
    }
    /* folded down: quieter, not as quiet as their sum (5.1's left is FL + C + LFE + BL) */
    scale = left > right ? left : right;
    if (scale > 1.0f)
    {
        scale = 1.0f / sqrtf( scale );
        for (c = 0; c < s->channels; c++)
        {
            s->weight[c][0] *= scale;
            s->weight[c][1] *= scale;
        }
    }
}

/* one frame of the ring, in stereo and at the program's volume */
static void read_frame( const struct stream *s, uint64_t index, float out[2] )
{
    const struct ps4_audio_ring *ring = s->ring;
    const unsigned char *p = PS4_AUDIO_DATA( ring ) + (size_t)(index % s->ring_frames) * s->frame_bytes;
    float left = 0, right = 0;
    uint32_t c;

    for (c = 0; c < s->channels; c++, p += s->sample_bytes)
    {
        float v;

        switch (s->format)
        {
        case PS4_AUDIO_U8:
            v = ((int)p[0] - 128) * (1.0f / 128);
            break;
        case PS4_AUDIO_S16:
        {
            int16_t x;
            memcpy( &x, p, 2 );
            v = x * (1.0f / 32768);
            break;
        }
        case PS4_AUDIO_S24:
            v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) * (1.0f / 2147483648.0f);
            break;
        case PS4_AUDIO_S32:
        {
            int32_t x;
            memcpy( &x, p, 4 );
            v = x * (1.0f / 2147483648.0f);
            break;
        }
        default:
            memcpy( &v, p, 4 );
            if (!isfinite( v )) v = 0;
            break;
        }
        v *= ring->volume[c];
        left += v * s->weight[c][0];
        right += v * s->weight[c][1];
    }
    out[0] = left;
    out[1] = right;
}

/* a running stream's frames for one grain of the port, added to mix */
static void mix_stream( struct stream *s, float *mix )
{
    struct ps4_audio_ring *ring = s->ring;
    const uint64_t written = __atomic_load_n( &ring->written, __ATOMIC_ACQUIRE );
    const uint64_t discard = __atomic_load_n( &ring->discard, __ATOMIC_ACQUIRE );
    uint64_t played = ring->played, available;
    unsigned int i = 0;

    if (played < discard)  /* (Reset: what it had is not played) */
    {
        played = discard;
        s->frac = 1.0;
        s->prev[0] = s->prev[1] = s->next[0] = s->next[1] = 0;
        __atomic_store_n( &ring->played, played, __ATOMIC_RELEASE );
    }
    if (!__atomic_load_n( &ring->running, __ATOMIC_ACQUIRE ) || written < played) return;
    available = written - played;
    if (available > s->ring_frames) available = s->ring_frames;

    if (s->rate == PS4_AUDIO_RATE)
    {
        for (; i < GRAIN && available; i++, available--)
        {
            float frame[2];

            read_frame( s, played++, frame );
            mix[2 * i] += frame[0];
            mix[2 * i + 1] += frame[1];
        }
    }
    else
    {
        const double step = (double)s->rate / PS4_AUDIO_RATE;

        for (; i < GRAIN; i++)
        {
            while (s->frac >= 1.0)
            {
                if (!available) goto done;
                s->prev[0] = s->next[0];
                s->prev[1] = s->next[1];
                read_frame( s, played++, s->next );
                available--;
                s->frac -= 1.0;
            }
            mix[2 * i] += s->prev[0] + (s->next[0] - s->prev[0]) * (float)s->frac;
            mix[2 * i + 1] += s->prev[1] + (s->next[1] - s->prev[1]) * (float)s->frac;
            s->frac += step;
        }
    }
done:
    if (i < GRAIN)
    {
        if (s->had_frames) ring->underruns++;
        s->had_frames = 0;
    }
    else s->had_frames = 1;
    __atomic_store_n( &ring->played, played, __ATOMIC_RELEASE );
}

static void *mixer_thread( void *arg )
{
    static float mix[GRAIN * 2];
    static int16_t out[GRAIN * 2];
    const int init = sceAudioOutInit();
    const int port = sceAudioOutOpen( USER_SYSTEM, PORT_MAIN, 0, GRAIN, PS4_AUDIO_RATE, S16_STEREO );
    double next = now_seconds(), last_report = next;
    int i;

    fprintf( stderr, "wineps4: audio: the PS4's port %s (init 0x%x, port 0x%x): programs' sound%s\n",
             port >= 0 ? "opened" : "did not open", init, port, port >= 0 ? " mixed into it" : " is not heard" );
    if (port >= 0)
    {
        int32_t volume[2] = { 32768, 32768 };  /* 0 dB, front left and right */
        sceAudioOutSetVolume( port, 0x3, volume );
    }
    for (;;)
    {
        memset( mix, 0, sizeof(mix) );
        pthread_mutex_lock( &streams_lock );
        for (i = 0; i < stream_count; i++) mix_stream( &streams[i], mix );
        pthread_mutex_unlock( &streams_lock );
        for (i = 0; i < GRAIN * 2; i++)
        {
            const float v = mix[i] * 32768.0f;
            out[i] = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
        }
        if (port >= 0) sceAudioOutOutput( port, out );  /* (blocks until the port takes it) */
        else  /* paced by the clock: the programs' streams go on, unheard */
        {
            struct timespec ts;
            double wait;

            next += (double)GRAIN / PS4_AUDIO_RATE;
            wait = next - now_seconds();
            if (wait < -0.1) next = now_seconds();
            else if (wait > 0)
            {
                ts.tv_sec = 0;
                ts.tv_nsec = (long)(wait * 1e9);
                nanosleep( &ts, NULL );
            }
        }

        if (audio_log && now_seconds() - last_report >= 1.0)
        {
            last_report = now_seconds();
            pthread_mutex_lock( &streams_lock );
            for (i = 0; i < stream_count; i++)
            {
                struct stream *s = &streams[i];
                const uint32_t underruns = s->ring->underruns;

                if (underruns == s->logged_underruns) continue;
                fprintf( stderr, "wineps4: audio: stream %u of %s ran dry %u times in a second (%u in all): "
                         "the program wrote late\n", s->id, s->name, underruns - s->logged_underruns, underruns );
                s->logged_underruns = underruns;
            }
            pthread_mutex_unlock( &streams_lock );
        }
    }
    return NULL;
}

static void start_mixer( void )
{
    pthread_attr_t attr;
    pthread_t thread;

    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, 256 * 1024 );
    if (!pthread_create( &thread, &attr, mixer_thread, NULL )) pthread_detach( thread );
    else fprintf( stderr, "wineps4: audio: no mixer thread: programs' sound is not heard\n" );
    pthread_attr_destroy( &attr );
}

static unsigned int sample_bytes( uint32_t format )
{
    switch (format)
    {
    case PS4_AUDIO_U8: return 1;
    case PS4_AUDIO_S16: return 2;
    case PS4_AUDIO_S24: return 3;
    case PS4_AUDIO_S32:
    case PS4_AUDIO_FLOAT: return 4;
    }
    return 0;
}

/* a program's new stream: its hello and shared memory, then the answer (0: taken) */
static void accept_stream( int server )
{
    union { struct cmsghdr align; char buf[CMSG_SPACE( sizeof(int) )]; } control;
    struct ps4_audio_hello hello;
    struct ps4_audio_ring *ring = MAP_FAILED, head;
    struct stream *s;
    struct msghdr m;
    struct cmsghdr *cmsg;
    struct iovec iov;
    struct pollfd pfd;
    const char *why = NULL;
    uint32_t reply = 2;
    ssize_t n = -1;
    int conn, fd = -1;

    if ((conn = accept( server, NULL, NULL )) == -1) return;
    fcntl( conn, F_SETFD, FD_CLOEXEC );
    pfd.fd = conn;
    pfd.events = POLLIN;
    if (poll( &pfd, 1, 1000 ) == 1)
    {
        memset( &m, 0, sizeof(m) );
        iov.iov_base = &hello;
        iov.iov_len = sizeof(hello);
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = control.buf;
        m.msg_controllen = sizeof(control.buf);
        while ((n = recvmsg( conn, &m, MSG_WAITALL )) == -1 && errno == EINTR) ;
        for (cmsg = CMSG_FIRSTHDR( &m ); n > 0 && cmsg; cmsg = CMSG_NXTHDR( &m, cmsg ))
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS && fd == -1 &&
                cmsg->cmsg_len >= CMSG_LEN( sizeof(int) ))
                memcpy( &fd, CMSG_DATA( cmsg ), sizeof(int) );
    }
    if (n != sizeof(hello) || hello.magic != PS4_AUDIO_MAGIC || fd == -1) why = "no stream";
    else if (hello.size < PS4_AUDIO_DATA_OFFSET || hello.size > MAX_RING) why = "its memory's size";
    else if ((ring = mmap( NULL, hello.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) == MAP_FAILED)
        why = "its memory did not map";
    else
    {
        memcpy( &head, ring, sizeof(head) );  /* (checked and kept as it is now) */
        head.name[sizeof(head.name) - 1] = 0;
        if (head.magic != PS4_AUDIO_MAGIC || head.version != PS4_AUDIO_VERSION) why = "another version";
        else if (!sample_bytes( head.format ) || !head.channels || head.channels > PS4_AUDIO_CHANNELS ||
                 head.rate < 1000 || head.rate > 384000 || head.frame_bytes < head.channels * sample_bytes( head.format ) ||
                 !head.ring_frames || PS4_AUDIO_DATA_OFFSET + (uint64_t)head.ring_frames * head.frame_bytes > hello.size)
            why = "its format";
    }
    if (fd != -1) close( fd );

    pthread_mutex_lock( &streams_lock );
    if (!why && stream_count == MAX_STREAMS) why = "too many streams";
    if (!why)
    {
        s = &streams[stream_count++];
        memset( s, 0, sizeof(*s) );
        s->sock = conn;
        s->ring = ring;
        s->size = hello.size;
        s->id = ++next_id;
        s->format = head.format;
        s->rate = head.rate;
        s->channels = head.channels;
        s->channel_mask = head.channel_mask;
        s->frame_bytes = head.frame_bytes;
        s->ring_frames = head.ring_frames;
        s->sample_bytes = sample_bytes( s->format );
        memcpy( s->name, head.name, sizeof(s->name) );
        s->frac = 1.0;
        speaker_weights( s );
        reply = 0;
    }
    pthread_mutex_unlock( &streams_lock );

    if (write( conn, &reply, sizeof(reply) ) != sizeof(reply) && !why)
        return;  /* (taken, and its program went: dropped when its socket reads 0) */
    if (why)
    {
        fprintf( stderr, "wineps4: audio: a stream refused: %s\n", why );
        if (ring != MAP_FAILED) munmap( ring, hello.size );
        close( conn );
        return;
    }
    if (audio_log)
        fprintf( stderr, "wineps4: audio: stream %u of %s: %u Hz, %u channels (mask 0x%x), format %u, %u frames ahead at most\n",
                 next_id, head.name, head.rate, head.channels, head.channel_mask, head.format, head.ring_frames );
    pthread_once( &mixer_once, start_mixer );
}

static void drop_stream( int sock )
{
    int i;

    pthread_mutex_lock( &streams_lock );
    for (i = 0; i < stream_count; i++)
    {
        struct stream *s = &streams[i];

        if (s->sock != sock) continue;
        if (audio_log)
            fprintf( stderr, "wineps4: audio: stream %u of %s ended (%llu frames played, ran dry %u times)\n",
                     s->id, s->name, (unsigned long long)s->ring->played, s->ring->underruns );
        munmap( s->ring, s->size );
        close( s->sock );
        streams[i] = streams[--stream_count];
        break;
    }
    pthread_mutex_unlock( &streams_lock );
}

static void *listen_thread( void *arg )
{
    const int server = (int)(intptr_t)arg;
    struct pollfd fds[1 + MAX_STREAMS];

    for (;;)
    {
        int n = 0, i;

        fds[n].fd = server;
        fds[n].events = POLLIN;
        fds[n++].revents = 0;
        pthread_mutex_lock( &streams_lock );
        for (i = 0; i < stream_count; i++)
        {
            fds[n].fd = streams[i].sock;
            fds[n].events = POLLIN;
            fds[n++].revents = 0;
        }
        pthread_mutex_unlock( &streams_lock );

        if (poll( fds, n, -1 ) == -1)
        {
            if (errno != EINTR) usleep( 100000 );
            continue;
        }
        for (i = 1; i < n; i++)
        {
            char byte;

            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
            /* (a stream sends nothing after its hello: readable is its end) */
            if (recv( fds[i].fd, &byte, 1, MSG_DONTWAIT ) == -1 && (errno == EAGAIN || errno == EINTR)) continue;
            drop_stream( fds[i].fd );
        }
        if (fds[0].revents & POLLIN) accept_stream( server );
    }
    return NULL;
}

/* the desktop's: the socket the programs' streams come to; the port opens with the first stream */
void ps4_audio_start( void )
{
    const char *log = getenv( "WINEPS4_AUDIO_LOG" );
    struct sockaddr_un addr;
    pthread_t thread;
    int server;

    audio_log = !log || strcmp( log, "0" );
    if ((server = socket( AF_UNIX, SOCK_STREAM, 0 )) == -1)
    {
        fprintf( stderr, "wineps4: audio: socket: %s: programs' sound is not heard\n", strerror( errno ) );
        return;
    }
    fcntl( server, F_SETFD, FD_CLOEXEC );
    memset( &addr, 0, sizeof(addr) );
    addr.sun_len = sizeof(addr);
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, PS4_AUDIO_SOCKET );
    unlink( PS4_AUDIO_SOCKET );
    if (bind( server, (struct sockaddr *)&addr, sizeof(addr) ) == -1 || listen( server, 16 ) == -1)
    {
        fprintf( stderr, "wineps4: audio: %s: %s: programs' sound is not heard\n", PS4_AUDIO_SOCKET, strerror( errno ) );
        close( server );
        return;
    }
    if (pthread_create( &thread, NULL, listen_thread, (void *)(intptr_t)server ))
    {
        fprintf( stderr, "wineps4: audio: no listening thread: programs' sound is not heard\n" );
        close( server );
        return;
    }
    pthread_detach( thread );
    if (audio_log) fprintf( stderr, "wineps4: audio: programs' streams at %s\n", PS4_AUDIO_SOCKET );
}
