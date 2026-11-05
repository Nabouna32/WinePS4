/*
 * wineps4.drv's audio driver: mmdevapi's streams to the desktop's mixer (WinePS4)
 *
 * mmdevapi loads it as "ps4" (HKCU\Software\Wine\Drivers, Audio). It follows wineoss.drv's oss.c:
 * the WASAPI buffer is the stream's own (local_buffer, in the program's memory where a 32-bit
 * program reaches it too), and the stream's timer thread copies what the program wrote into the
 * ring the desktop plays from (port/include/ps4_audio.h, port/ps4_audio_desk.c) as the OSS driver
 * writes to /dev/dsp, a few periods ahead of the desktop. Frames count as played once the desktop
 * took them from the ring. Nothing to record from: no capture endpoints, and no MIDI devices.
 *
 * Copyright 2011 Andrew Eikum for CodeWeavers
 *           2022 Huw Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "initguid.h"
#include "audioclient.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "../mmdevapi/unixlib.h"
#include "unixlib.h"
#include "ps4_audio.h"

WINE_DEFAULT_DEBUG_CHANNEL(ps4audio);

#ifdef __PS4_WINE__
extern int ps4_anon_shm( unsigned long long size );  /* port/ps4_wine_port.c */
#endif

extern NTSTATUS ps4drv_unix_init( void *arg );  /* ps4drv_main.c: the display driver's */

struct ps4_stream
{
    WAVEFORMATEX *fmt;
    EDataFlow flow;
    UINT flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;

    int sock;                       /* to the desktop, held as long as the stream */
    struct ps4_audio_ring *ring;    /* what the desktop plays; NULL: no desktop, frames played by the clock */
    BYTE *ring_data;
    size_t ring_size;
    UINT32 ring_target;             /* frames kept in the ring ahead of the desktop */

    BOOL playing, please_quit;
    UINT64 written_frames, last_pos_frames;
    UINT32 period_frames, bufsize_frames, held_frames, tmp_buffer_frames, in_ring_frames;
    UINT32 lcl_offs_frames; /* offs into local_buffer where valid data starts */
    REFERENCE_TIME period;

    BYTE *local_buffer, *tmp_buffer;
    INT32 getbuf_last; /* <0 when using tmp_buffer */

    pthread_mutex_t lock;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;
/* the ring holds this much ahead of the desktop at least: a program's timer thread late by less
 * than that is not heard */
#define RING_AHEAD_MS 50

static void ps4_write_data( struct ps4_stream *stream );

static ULONG_PTR zero_bits = 0;
static int audio_log = 1;  /* Settings > Diagnostics > Audio log: streams in wine.log (WINEPS4_AUDIO_LOG=0: off) */
static unsigned int stream_count;

static NTSTATUS ps4audio_not_implemented( void *args )
{
    return STATUS_SUCCESS;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static void ps4_lock( struct ps4_stream *stream )
{
    pthread_mutex_lock( &stream->lock );
}

static void ps4_unlock( struct ps4_stream *stream )
{
    pthread_mutex_unlock( &stream->lock );
}

static NTSTATUS ps4_unlock_result( struct ps4_stream *stream, HRESULT *result, HRESULT value )
{
    *result = value;
    ps4_unlock( stream );
    return STATUS_SUCCESS;
}

static struct ps4_stream *handle_get_stream( stream_handle h )
{
    return (struct ps4_stream *)(UINT_PTR)h;
}

static NTSTATUS ps4audio_process_attach( void *args )
{
    const char *log = getenv( "WINEPS4_AUDIO_LOG" );

    audio_log = !log || strcmp( log, "0" );
#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset)
    {
        SYSTEM_BASIC_INFORMATION info;

        NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL );
        zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    }
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_main_loop( void *args )
{
    struct main_loop_params *params = args;
    NtSetEvent( params->event, NULL );
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_test_connect( void *args )
{
    struct test_connect_params *params = args;

    /* (without the desktop's mixer a stream still runs, by the clock and unheard) */
    params->priority = Priority_Preferred;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_get_endpoint_ids( void *args )
{
    static const WCHAR name[] = {'P','S','4',' ','a','u','d','i','o',0};
    static const char device[] = "ps4";
    struct get_endpoint_ids_params *params = args;
    unsigned int offset = sizeof(*params->endpoints), needed;

    params->num = 0;
    params->default_idx = 0;
    if (params->flow != eRender)  /* nothing to record from */
    {
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    needed = offset + sizeof(name) + ((sizeof(device) + 1) & ~1);
    params->num = 1;
    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        return STATUS_SUCCESS;
    }
    params->endpoints[0].name = offset;
    memcpy( (char *)params->endpoints + offset, name, sizeof(name) );
    offset += sizeof(name);
    params->endpoints[0].device = offset;
    memcpy( (char *)params->endpoints + offset, device, sizeof(device) );
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT get_channel_mask( unsigned int channels )
{
    switch (channels)
    {
    case 0:
        return 0;
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 3:
        return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;    /* not _SURROUND */
    case 5:
        return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
    case 7:
        return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
    }
    FIXME( "Unknown speaker configuration: %u\n", channels );
    return 0;
}

/* the ring's sample format for the program's, -1: none the desktop mixes */
static int get_ps4_format( const WAVEFORMATEX *fmt )
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if (fmt->wFormatTag == WAVE_FORMAT_PCM ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM )))
    {
        switch (fmt->wBitsPerSample)
        {
        case 8: return PS4_AUDIO_U8;
        case 16: return PS4_AUDIO_S16;
        case 24: return PS4_AUDIO_S24;
        case 32: return PS4_AUDIO_S32;
        }
        return -1;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT )))
        return fmt->wBitsPerSample == 32 ? PS4_AUDIO_FLOAT : -1;
    return -1;
}

static WAVEFORMATEXTENSIBLE *clone_format( const WAVEFORMATEX *fmt )
{
    WAVEFORMATEXTENSIBLE *ret;
    size_t size;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc( size );
    if (!ret)
        return NULL;

    memcpy( ret, fmt, size );

    ret->Format.cbSize = size - sizeof(WAVEFORMATEX);

    return ret;
}

/* what setup_oss_device checks of a format: the desktop mixes any rate, 1 to 8 channels and the
 * PCM and float formats, so all are taken as they are (S_FALSE: not quite as Windows writes them) */
static HRESULT check_format( AUDCLNT_SHAREMODE share, const WAVEFORMATEX *fmt )
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
    HRESULT ret = S_OK;

    if (get_ps4_format( fmt ) < 0)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        (fmtex->Format.nAvgBytesPerSec == 0 ||
         fmtex->Format.nBlockAlign == 0 ||
         fmtex->Samples.wValidBitsPerSample > fmtex->Format.wBitsPerSample))
        return E_INVALIDARG;

    if (fmt->nChannels == 0 || fmt->nChannels > PS4_AUDIO_CHANNELS ||
        fmt->nSamplesPerSec < 1000 || fmt->nSamplesPerSec > 384000 ||
        fmt->nBlockAlign < fmt->nChannels * fmt->wBitsPerSample / 8)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (fmt->nBlockAlign != fmt->nChannels * fmt->wBitsPerSample / 8 ||
        fmt->nAvgBytesPerSec != fmt->nBlockAlign * fmt->nSamplesPerSec ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         fmtex->Samples.wValidBitsPerSample < fmtex->Format.wBitsPerSample))
        ret = S_FALSE;

    if (share == AUDCLNT_SHAREMODE_EXCLUSIVE && fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        if (fmtex->dwChannelMask == 0 || fmtex->dwChannelMask & SPEAKER_RESERVED)
            ret = S_FALSE;
    }

    TRACE( "returning: %08x\n", (unsigned)ret );
    return ret;
}

/* the desktop's ring for the stream: shared memory sent over its socket. FALSE: no desktop */
static BOOL open_ring( struct ps4_stream *stream, int format, const WCHAR *name )
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)stream->fmt;
    union { struct cmsghdr align; char buf[CMSG_SPACE( sizeof(int) )]; } control;
    struct ps4_audio_hello hello = { PS4_AUDIO_MAGIC };
    struct ps4_audio_ring *ring;
    struct sockaddr_un addr;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec iov;
    struct pollfd pfd;
    UINT32 ring_frames, reply = 1, i;
    size_t size;
    int fd = -1, sock = -1;

    stream->ring_target = max( 4 * stream->period_frames, stream->fmt->nSamplesPerSec * RING_AHEAD_MS / 1000 );
    ring_frames = 2 * stream->ring_target;
    size = PS4_AUDIO_DATA_OFFSET + (size_t)ring_frames * stream->fmt->nBlockAlign;
    size = (size + 0x3fff) & ~(size_t)0x3fff;  /* (the PS4's pages) */

#ifdef __PS4_WINE__
    fd = ps4_anon_shm( size );
#endif
    if (fd == -1) goto failed;
    ring = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (ring == MAP_FAILED) goto failed;

    memset( ring, 0, PS4_AUDIO_DATA_OFFSET );
    ring->magic        = PS4_AUDIO_MAGIC;
    ring->version      = PS4_AUDIO_VERSION;
    ring->format       = format;
    ring->rate         = stream->fmt->nSamplesPerSec;
    ring->channels     = stream->fmt->nChannels;
    ring->channel_mask = stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmtex->dwChannelMask ?
                         fmtex->dwChannelMask : get_channel_mask( stream->fmt->nChannels );
    ring->frame_bytes  = stream->fmt->nBlockAlign;
    ring->ring_frames  = ring_frames;
    for (i = 0; i < PS4_AUDIO_CHANNELS; i++) ring->volume[i] = 1.0f;
    for (i = 0; name && name[i] && i < sizeof(ring->name) - 1; i++)
        ring->name[i] = name[i] < 0x80 ? name[i] : '?';

    if ((sock = socket( AF_UNIX, SOCK_STREAM, 0 )) == -1) goto unmap;
    fcntl( sock, F_SETFD, FD_CLOEXEC );
    memset( &addr, 0, sizeof(addr) );
    addr.sun_len = sizeof(addr);
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, PS4_AUDIO_SOCKET );
    if (connect( sock, (struct sockaddr *)&addr, sizeof(addr) ) == -1) goto unmap;

    hello.size = size;
    memset( &msg, 0, sizeof(msg) );
    iov.iov_base = &hello;
    iov.iov_len = sizeof(hello);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    memset( &control, 0, sizeof(control) );
    msg.msg_control = control.buf;
    msg.msg_controllen = CMSG_SPACE( sizeof(int) );
    cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN( sizeof(int) );
    memcpy( CMSG_DATA( cmsg ), &fd, sizeof(int) );
    if (sendmsg( sock, &msg, 0 ) != sizeof(hello)) goto unmap;

    pfd.fd = sock;
    pfd.events = POLLIN;
    if (poll( &pfd, 1, 2000 ) != 1 || read( sock, &reply, sizeof(reply) ) != sizeof(reply) || reply) goto unmap;

    close( fd );
    stream->sock = sock;
    stream->ring = ring;
    stream->ring_data = PS4_AUDIO_DATA( ring );
    stream->ring_size = size;
    return TRUE;

unmap:
    munmap( ring, size );
failed:
    if (audio_log)
        fprintf( stderr, "wineps4: audio: no way to the desktop's mixer (%s): this stream plays unheard\n",
                 fd == -1 ? "no shared memory" : reply == 1 ? strerror( errno ) : "it refused the stream" );
    if (sock != -1) close( sock );
    if (fd != -1) close( fd );
    return FALSE;
}

static NTSTATUS ps4audio_create_stream( void *args )
{
    struct create_stream_params *params = args;
    WAVEFORMATEXTENSIBLE *fmtex;
    struct ps4_stream *stream;
    SIZE_T size;
    int format;

    params->result = S_OK;

    if (params->flow != eRender)
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (FAILED(params->result = check_format( params->share, params->fmt )))
        return STATUS_SUCCESS;
    params->result = S_OK;
    format = get_ps4_format( params->fmt );

    stream = calloc( 1, sizeof(*stream) );
    if (!stream)
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    stream->flow = params->flow;
    stream->sock = -1;
    pthread_mutex_init( &stream->lock, NULL );

    fmtex = clone_format( params->fmt );
    if (!fmtex)
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }
    stream->fmt = &fmtex->Format;

    stream->period = params->period;
    stream->period_frames = muldiv( params->fmt->nSamplesPerSec, params->period, 10000000 );

    stream->bufsize_frames = muldiv( params->duration, params->fmt->nSamplesPerSec, 10000000 );
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;
    size = stream->bufsize_frames * params->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory( GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                                 &size, MEM_COMMIT, PAGE_READWRITE ))
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }

    stream->share = params->share;
    stream->flags = params->flags;

    open_ring( stream, format, params->name );
    if (audio_log)
        fprintf( stderr, "wineps4: audio: stream %u%s: %u Hz, %u channels, %u bits%s, a buffer of %u frames, %u ahead in the ring\n",
                 ++stream_count, stream->ring ? "" : " (unheard)", params->fmt->nSamplesPerSec,
                 params->fmt->nChannels, params->fmt->wBitsPerSample, format == PS4_AUDIO_FLOAT ? " float" : "",
                 stream->bufsize_frames, stream->ring_target );

exit:
    if (FAILED(params->result))
    {
        if (stream->local_buffer)
        {
            size = 0;
            NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE );
        }
        pthread_mutex_destroy( &stream->lock );
        free( stream->fmt );
        free( stream );
    }
    else
    {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    SIZE_T size;

    if (params->timer_thread)
    {
        stream->please_quit = TRUE;
        NtWaitForSingleObject( params->timer_thread, FALSE, NULL );
        NtClose( params->timer_thread );
    }

    if (stream->ring) munmap( stream->ring, stream->ring_size );
    if (stream->sock != -1) close( stream->sock );  /* (the desktop drops the stream) */
    if (stream->local_buffer)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE );
    }
    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
    }
    free( stream->fmt );
    pthread_mutex_destroy( &stream->lock );
    free( stream );

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_start( void *args )
{
    struct start_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET );

    if (stream->playing)
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_NOT_STOPPED );

    stream->playing = TRUE;
    if (stream->ring)
    {
        __atomic_store_n( &stream->ring->running, 1, __ATOMIC_RELEASE );
        if (stream->held_frames) ps4_write_data( stream );  /* (heard now, not at the timer's next round) */
    }

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_stop( void *args )
{
    struct stop_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    if (!stream->playing)
        return ps4_unlock_result( stream, &params->result, S_FALSE );

    /* (what is in the ring stays there, played when it starts again) */
    stream->playing = FALSE;
    if (stream->ring) __atomic_store_n( &stream->ring->running, 0, __ATOMIC_RELEASE );

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_reset( void *args )
{
    struct reset_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    if (stream->playing)
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_NOT_STOPPED );

    if (stream->getbuf_last)
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING );

    stream->written_frames = 0;
    stream->last_pos_frames = 0;
    stream->held_frames = 0;
    stream->lcl_offs_frames = 0;
    stream->in_ring_frames = 0;
    if (stream->ring)  /* the desktop skips what is in the ring */
        __atomic_store_n( &stream->ring->discard, stream->ring->written, __ATOMIC_RELEASE );

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static void silence_buffer( struct ps4_stream *stream, BYTE *buffer, UINT32 frames )
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE *)stream->fmt;
    if ((stream->fmt->wFormatTag == WAVE_FORMAT_PCM ||
         (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          IsEqualGUID( &fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM ))) &&
        stream->fmt->wBitsPerSample == 8)
        memset( buffer, 128, frames * stream->fmt->nBlockAlign );
    else
        memset( buffer, 0, frames * stream->fmt->nBlockAlign );
}

/* frames of the local buffer into the ring, after what the program wrote before */
static void ring_put( struct ps4_stream *stream, const BYTE *src, UINT32 frames )
{
    struct ps4_audio_ring *ring = stream->ring;
    const UINT32 align = stream->fmt->nBlockAlign;
    const UINT64 written = ring->written;  /* (only this side moves it) */
    const UINT32 pos = written % ring->ring_frames, chunk = min( frames, ring->ring_frames - pos );

    memcpy( stream->ring_data + (size_t)pos * align, src, (size_t)chunk * align );
    memcpy( stream->ring_data, src + (size_t)chunk * align, (size_t)(frames - chunk) * align );
    __atomic_store_n( &ring->written, written + frames, __ATOMIC_RELEASE );
}

/* oss_write_data with the ring for the OSS device: what the desktop took since the last time is
 * played, and the ring gets the frames after what it has, up to ring_target */
static void ps4_write_data( struct ps4_stream *stream )
{
    struct ps4_audio_ring *ring = stream->ring;
    UINT32 in_ring_frames, write_limit, write_offs_frames, new_frames, to_write_frames;
    UINT64 played, discard;
    SIZE_T advanced;

    if (!ring)  /* no desktop: a period's frames played a period */
    {
        advanced = min( stream->held_frames, stream->period_frames );
        stream->lcl_offs_frames = (stream->lcl_offs_frames + advanced) % stream->bufsize_frames;
        stream->held_frames -= advanced;
        return;
    }

    played = __atomic_load_n( &ring->played, __ATOMIC_ACQUIRE );
    discard = ring->discard;
    if (played < discard) played = discard;
    in_ring_frames = ring->written - played;
    if (in_ring_frames > stream->in_ring_frames)
        in_ring_frames = stream->in_ring_frames;

    /*        vvvvvv - in_ring_frames
     * [--xxxxxxxxxx]
     *       [xxxxxxxxxx--]
     *        ^^^^^^^^^^ - held_frames
     *        ^ - lcl_offs_frames
     */
    advanced = stream->in_ring_frames - in_ring_frames;
    if (advanced > stream->held_frames)
        advanced = stream->held_frames;
    stream->lcl_offs_frames += advanced;
    stream->lcl_offs_frames %= stream->bufsize_frames;
    stream->held_frames -= advanced;
    stream->in_ring_frames = in_ring_frames;
    TRACE( "advanced by %lu, lcl_offs: %u, held: %u, in_ring: %u\n",
           advanced, stream->lcl_offs_frames, stream->held_frames, stream->in_ring_frames );

    if (stream->held_frames == stream->in_ring_frames || stream->in_ring_frames >= stream->ring_target)
        return;
    write_limit = stream->ring_target - stream->in_ring_frames;

    write_offs_frames = (stream->lcl_offs_frames + stream->in_ring_frames) % stream->bufsize_frames;
    new_frames = min( stream->held_frames - stream->in_ring_frames, write_limit );

    to_write_frames = min( new_frames, stream->bufsize_frames - write_offs_frames );
    ring_put( stream, stream->local_buffer + (size_t)write_offs_frames * stream->fmt->nBlockAlign, to_write_frames );
    if (new_frames > to_write_frames)  /* wrapped: the rest from the start */
        ring_put( stream, stream->local_buffer, new_frames - to_write_frames );
    stream->in_ring_frames += new_frames;
}

static NTSTATUS ps4audio_timer_loop( void *args )
{
    struct timer_loop_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    LARGE_INTEGER delay, now, next;
    int adjust;

    ps4_lock( stream );

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter( &now, NULL );
    next.QuadPart = now.QuadPart + stream->period;

    while (!stream->please_quit)
    {
        if (stream->playing && stream->held_frames)
            ps4_write_data( stream );
        if (stream->event)
            NtSetEvent( stream->event, NULL );
        ps4_unlock( stream );

        NtDelayExecution( FALSE, &delay );

        ps4_lock( stream );
        NtQueryPerformanceCounter( &now, NULL );
        adjust = next.QuadPart - now.QuadPart;
        if (adjust > stream->period / 2)
            adjust = stream->period / 2;
        else if (adjust < -stream->period / 2)
            adjust = -stream->period / 2;
        delay.QuadPart = -(stream->period + adjust);
        next.QuadPart += stream->period;
    }

    ps4_unlock( stream );

    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_get_render_buffer( void *args )
{
    struct get_render_buffer_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    UINT32 write_pos, frames = params->frames;
    BYTE **data = params->data;
    SIZE_T size;

    ps4_lock( stream );

    if (stream->getbuf_last)
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (!frames)
        return ps4_unlock_result( stream, &params->result, S_OK );

    if (stream->held_frames + frames > stream->bufsize_frames)
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_BUFFER_TOO_LARGE );

    write_pos = (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    if (write_pos + frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
                stream->tmp_buffer = NULL;
            }
            size = frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory( GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                         &size, MEM_COMMIT, PAGE_READWRITE ))
            {
                stream->tmp_buffer_frames = 0;
                return ps4_unlock_result( stream, &params->result, E_OUTOFMEMORY );
            }
            stream->tmp_buffer_frames = frames;
        }
        *data = stream->tmp_buffer;
        stream->getbuf_last = -frames;
    }
    else
    {
        *data = stream->local_buffer + write_pos * stream->fmt->nBlockAlign;
        stream->getbuf_last = frames;
    }

    silence_buffer( stream, *data, frames );

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static void ps4_wrap_buffer( struct ps4_stream *stream, BYTE *buffer, UINT32 written_frames )
{
    UINT32 write_offs_frames = (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    UINT32 write_offs_bytes = write_offs_frames * stream->fmt->nBlockAlign;
    UINT32 chunk_frames = stream->bufsize_frames - write_offs_frames;
    UINT32 chunk_bytes = chunk_frames * stream->fmt->nBlockAlign;
    UINT32 written_bytes = written_frames * stream->fmt->nBlockAlign;

    if (written_bytes <= chunk_bytes)
    {
        memcpy( stream->local_buffer + write_offs_bytes, buffer, written_bytes );
    }
    else
    {
        memcpy( stream->local_buffer + write_offs_bytes, buffer, chunk_bytes );
        memcpy( stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes );
    }
}

static NTSTATUS ps4audio_release_render_buffer( void *args )
{
    struct release_render_buffer_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    UINT32 written_frames = params->written_frames;
    UINT flags = params->flags;
    BYTE *buffer;

    ps4_lock( stream );

    if (!written_frames)
    {
        stream->getbuf_last = 0;
        return ps4_unlock_result( stream, &params->result, S_OK );
    }

    if (!stream->getbuf_last)
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_INVALID_SIZE );

    if (stream->getbuf_last >= 0)
        buffer = stream->local_buffer + stream->fmt->nBlockAlign *
                 ((stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames);
    else
        buffer = stream->tmp_buffer;

    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer( stream, buffer, written_frames );

    if (stream->getbuf_last < 0)
        ps4_wrap_buffer( stream, buffer, written_frames );

    stream->held_frames += written_frames;
    stream->written_frames += written_frames;
    stream->getbuf_last = 0;
    /* into the ring at once: the desktop has them even when this stream's timer thread is late */
    if (stream->playing && stream->ring) ps4_write_data( stream );

    return ps4_unlock_result( stream, &params->result, S_OK );
}

/* (no capture streams: never called) */
static NTSTATUS ps4audio_get_capture_buffer( void *args )
{
    struct get_capture_buffer_params *params = args;  /* (result is where it is in the 32-bit one too) */

    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_release_capture_buffer( void *args )
{
    struct release_capture_buffer_params *params = args;

    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_is_format_supported( void *args )
{
    struct is_format_supported_params *params = args;

    if (params->flow != eRender)
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else
        params->result = check_format( params->share, params->fmt_in );
    return STATUS_SUCCESS;
}

/* 48 kHz float stereo: the desktop's port, nothing to convert */
static NTSTATUS ps4audio_get_mix_format( void *args )
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if (params->flow != eRender)
    {
        params->result = params->flow == eCapture ? AUDCLNT_E_DEVICE_INVALIDATED : E_UNEXPECTED;
        return STATUS_SUCCESS;
    }

    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.wBitsPerSample = 32;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fmt->Format.nChannels = 2;
    fmt->Format.nSamplesPerSec = PS4_AUDIO_RATE;
    fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    fmt->Format.nBlockAlign = (fmt->Format.wBitsPerSample * fmt->Format.nChannels) / 8;
    fmt->Format.nAvgBytesPerSec = fmt->Format.nSamplesPerSec * fmt->Format.nBlockAlign;
    fmt->Samples.wValidBitsPerSample = fmt->Format.wBitsPerSample;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_get_device_period( void *args )
{
    struct get_device_period_params *params = args;

    if (params->def_period)
        *params->def_period = def_period;
    if (params->min_period)
        *params->min_period = min_period;

    params->result = S_OK;

    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_get_buffer_size( void *args )
{
    struct get_buffer_size_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    *params->frames = stream->bufsize_frames;

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_get_latency( void *args )
{
    struct get_latency_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    /* pretend we process audio in Period chunks, so max latency includes
     * the period time.  Some native machines add .6666ms in shared mode. */
    *params->latency = stream->period + 6666;

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_get_current_padding( void *args )
{
    struct get_current_padding_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    *params->padding = stream->held_frames;

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_get_next_packet_size( void *args )
{
    struct get_next_packet_size_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    UINT32 *frames = params->frames;

    ps4_lock( stream );

    *frames = stream->held_frames < stream->period_frames ? 0 : stream->period_frames;

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_get_frequency( void *args )
{
    struct get_frequency_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    UINT64 *freq = params->freq;

    ps4_lock( stream );

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *freq = stream->fmt->nSamplesPerSec;

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_get_position( void *args )
{
    struct get_position_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    UINT64 *pos = params->pos, *qpctime = params->qpctime;

    if (params->device)
    {
        FIXME( "Device position reporting not implemented\n" );
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    ps4_lock( stream );

    *pos = stream->written_frames - stream->held_frames;
    if (*pos < stream->last_pos_frames)
        *pos = stream->last_pos_frames;

    stream->last_pos_frames = *pos;

    TRACE( "returning: %s\n", wine_dbgstr_longlong( *pos ) );
    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *pos *= stream->fmt->nBlockAlign;

    if (qpctime)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter( &stamp, &freq );
        *qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return ps4_unlock_result( stream, &params->result, S_OK );
}

/* the desktop applies each channel's volume as it mixes */
static NTSTATUS ps4audio_set_volumes( void *args )
{
    struct set_volumes_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );
    UINT16 i;

    ps4_lock( stream );
    if (stream->ring)
        for (i = 0; i < stream->fmt->nChannels && i < PS4_AUDIO_CHANNELS; ++i)
            stream->ring->volume[i] = params->master_volume * params->volumes[i] * params->session_volumes[i];
    ps4_unlock( stream );

    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_set_event_handle( void *args )
{
    struct set_event_handle_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        return ps4_unlock_result( stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED );

    if (stream->event)
    {
        FIXME( "called twice\n" );
        return ps4_unlock_result( stream, &params->result, HRESULT_FROM_WIN32(ERROR_INVALID_NAME) );
    }

    stream->event = params->event;

    return ps4_unlock_result( stream, &params->result, S_OK );
}

static NTSTATUS ps4audio_set_sample_rate( void *args )
{
    struct set_sample_rate_params *params = args;

    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_is_started( void *args )
{
    struct is_started_params *params = args;
    struct ps4_stream *stream = handle_get_stream( params->stream );

    ps4_lock( stream );

    return ps4_unlock_result( stream, &params->result, stream->playing ? S_OK : S_FALSE );
}

static NTSTATUS ps4audio_get_prop_value( void *args )
{
    struct get_prop_value_params *params = args;

    params->result = E_NOTIMPL;

    return STATUS_SUCCESS;
}

/* MIDI and aux: no devices. mmdevapi's MIDI notify thread waits in midi_notify_wait until
 * midi_release */

static pthread_mutex_t midi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t midi_cond = PTHREAD_COND_INITIALIZER;
static BOOL midi_quit;

static NTSTATUS ps4audio_midi_init( void *args )
{
    struct midi_init_params *params = args;

    pthread_mutex_lock( &midi_lock );
    midi_quit = FALSE;
    pthread_mutex_unlock( &midi_lock );
    *params->err = DRV_SUCCESS;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_midi_release( void *args )
{
    pthread_mutex_lock( &midi_lock );
    midi_quit = TRUE;
    pthread_cond_broadcast( &midi_cond );
    pthread_mutex_unlock( &midi_lock );
    return STATUS_SUCCESS;
}

static UINT midi_message( UINT msg, UINT num_devs_msg )
{
    switch (msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        return MMSYSERR_NOERROR;
    }
    return msg == num_devs_msg ? 0 : MMSYSERR_BADDEVICEID;
}

static NTSTATUS ps4audio_midi_out_message( void *args )
{
    struct midi_out_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = midi_message( params->msg, MODM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_midi_in_message( void *args )
{
    struct midi_in_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = midi_message( params->msg, MIDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_midi_notify_wait( void *args )
{
    struct midi_notify_wait_params *params = args;

    pthread_mutex_lock( &midi_lock );
    while (!midi_quit) pthread_cond_wait( &midi_cond, &midi_lock );
    pthread_mutex_unlock( &midi_lock );
    *params->quit = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_aux_message( void *args )
{
    struct aux_message_params *params = args;

    *params->err = midi_message( params->msg, AUXDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

/* the unix calls: mmdevapi's, in its order (../mmdevapi/unixlib.h), then the display driver's */
const unixlib_entry_t __wine_unix_call_funcs[] =
{
    ps4audio_process_attach,
    ps4audio_not_implemented,
    ps4audio_main_loop,
    ps4audio_get_endpoint_ids,
    ps4audio_create_stream,
    ps4audio_release_stream,
    ps4audio_start,
    ps4audio_stop,
    ps4audio_reset,
    ps4audio_timer_loop,
    ps4audio_get_render_buffer,
    ps4audio_release_render_buffer,
    ps4audio_get_capture_buffer,
    ps4audio_release_capture_buffer,
    ps4audio_is_format_supported,
    ps4audio_not_implemented,
    ps4audio_get_mix_format,
    ps4audio_get_device_period,
    ps4audio_get_buffer_size,
    ps4audio_get_latency,
    ps4audio_get_current_padding,
    ps4audio_get_next_packet_size,
    ps4audio_get_frequency,
    ps4audio_get_position,
    ps4audio_set_volumes,
    ps4audio_set_event_handle,
    ps4audio_set_sample_rate,
    ps4audio_test_connect,
    ps4audio_is_started,
    ps4audio_get_prop_value,
    ps4audio_not_implemented,
    ps4audio_midi_init,
    ps4audio_midi_release,
    ps4audio_midi_out_message,
    ps4audio_midi_in_message,
    ps4audio_midi_notify_wait,
    ps4audio_aux_message,
    ps4drv_unix_init,
};

C_ASSERT( ps4drv_unix_func_init == funcs_count );
C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == ps4drv_unix_func_count );

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS ps4audio_wow64_test_connect( void *args )
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name),
    };
    ps4audio_test_connect( &params );
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_main_loop( void *args )
{
    struct
    {
        PTR32 event;
    } *params32 = args;
    struct main_loop_params params =
    {
        .event = ULongToHandle(params32->event)
    };
    return ps4audio_main_loop( &params );
}

static NTSTATUS ps4audio_wow64_get_endpoint_ids( void *args )
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    ps4audio_get_endpoint_ids( &params );
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_create_stream( void *args )
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        UINT flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    ps4audio_create_stream( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_release_stream( void *args )
{
    struct
    {
        stream_handle stream;
        PTR32 timer_thread;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
        .timer_thread = ULongToHandle(params32->timer_thread)
    };
    ps4audio_release_stream( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_render_buffer( void *args )
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    ps4audio_get_render_buffer( &params );
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_is_format_supported( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    ps4audio_is_format_supported( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_mix_format( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    ps4audio_get_mix_format( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_device_period( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    ps4audio_get_device_period( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_buffer_size( void *args )
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    ps4audio_get_buffer_size( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_latency( void *args )
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    ps4audio_get_latency( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_current_padding( void *args )
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    ps4audio_get_current_padding( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_next_packet_size( void *args )
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    ps4audio_get_next_packet_size( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_frequency( void *args )
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    ps4audio_get_frequency( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_position( void *args )
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    ps4audio_get_position( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_set_volumes( void *args )
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return ps4audio_set_volumes( &params );
}

static NTSTATUS ps4audio_wow64_set_event_handle( void *args )
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };

    ps4audio_set_event_handle( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_get_prop_value( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer; /* caller allocated buffer to hold value's strings */
        PTR32 buffer_size;
    } *params32 = args;

    params32->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_midi_init( void *args )
{
    struct
    {
        PTR32 err;
    } *params32 = args;
    struct midi_init_params params =
    {
        .err = ULongToPtr(params32->err)
    };
    return ps4audio_midi_init( &params );
}

static NTSTATUS ps4audio_wow64_midi_out_message( void *args )
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
        PTR32 notify;
    } *params32 = args;

    *(BOOL *)ULongToPtr(params32->notify) = FALSE;  /* (send_notify: the 32-bit context's first) */
    *(UINT *)ULongToPtr(params32->err) = midi_message( params32->msg, MODM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_midi_in_message( void *args )
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
        PTR32 notify;
    } *params32 = args;

    *(BOOL *)ULongToPtr(params32->notify) = FALSE;
    *(UINT *)ULongToPtr(params32->err) = midi_message( params32->msg, MIDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps4audio_wow64_midi_notify_wait( void *args )
{
    struct
    {
        PTR32 quit;
        PTR32 notify;
    } *params32 = args;
    struct midi_notify_wait_params params =
    {
        .quit = ULongToPtr(params32->quit)
    };
    return ps4audio_midi_notify_wait( &params );
}

static NTSTATUS ps4audio_wow64_aux_message( void *args )
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
    } *params32 = args;
    struct aux_message_params params =
    {
        .dev_id = params32->dev_id,
        .msg = params32->msg,
        .user = params32->user,
        .param_1 = params32->param_1,
        .param_2 = params32->param_2,
        .err = ULongToPtr(params32->err),
    };
    return ps4audio_aux_message( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    ps4audio_process_attach,
    ps4audio_not_implemented,
    ps4audio_wow64_main_loop,
    ps4audio_wow64_get_endpoint_ids,
    ps4audio_wow64_create_stream,
    ps4audio_wow64_release_stream,
    ps4audio_start,
    ps4audio_stop,
    ps4audio_reset,
    ps4audio_timer_loop,
    ps4audio_wow64_get_render_buffer,
    ps4audio_release_render_buffer,
    ps4audio_get_capture_buffer,
    ps4audio_release_capture_buffer,
    ps4audio_wow64_is_format_supported,
    ps4audio_not_implemented,
    ps4audio_wow64_get_mix_format,
    ps4audio_wow64_get_device_period,
    ps4audio_wow64_get_buffer_size,
    ps4audio_wow64_get_latency,
    ps4audio_wow64_get_current_padding,
    ps4audio_wow64_get_next_packet_size,
    ps4audio_wow64_get_frequency,
    ps4audio_wow64_get_position,
    ps4audio_wow64_set_volumes,
    ps4audio_wow64_set_event_handle,
    ps4audio_set_sample_rate,
    ps4audio_wow64_test_connect,
    ps4audio_is_started,
    ps4audio_wow64_get_prop_value,
    ps4audio_not_implemented,
    ps4audio_wow64_midi_init,
    ps4audio_midi_release,
    ps4audio_wow64_midi_out_message,
    ps4audio_wow64_midi_in_message,
    ps4audio_wow64_midi_notify_wait,
    ps4audio_wow64_aux_message,
    ps4drv_unix_init,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == ps4drv_unix_func_count );

#endif /* _WIN64 */
