/* WinePS4: programs' sound, mixed by the desktop into the PS4's audio port.
 *
 * wineps4.drv is also Wine's audio driver ("ps4": HKCU\Software\Wine\Drivers, Audio). mmdevapi's
 * streams (WASAPI, and through it DirectSound, winmm's wave out, XAudio2, Media Foundation) each give
 * the desktop a ring of frames in shared memory: the program's audio.c writes frames into it from its
 * WASAPI buffer, the desktop's mixer (port/ps4_audio_desk.c) takes them, converts them to 48 kHz
 * stereo and plays the sum through one sceAudioOut port. Each side moves only its own count. The
 * stream's socket to the desktop carries the memory once and stays open as long as the stream: the
 * desktop drops the stream when it closes (the program ended). */
#ifndef PS4_AUDIO_H
#define PS4_AUDIO_H

#include <stdint.h>

#define PS4_AUDIO_SOCKET   "/data/WinePS4/.audio"
#define PS4_AUDIO_MAGIC    0x55413457  /* "W4AU" */
#define PS4_AUDIO_VERSION  1
#define PS4_AUDIO_RATE     48000       /* the port's */
#define PS4_AUDIO_CHANNELS 8           /* a stream's, at most */

enum ps4_audio_format
{
    PS4_AUDIO_U8 = 1,
    PS4_AUDIO_S16,
    PS4_AUDIO_S24,  /* 3 bytes */
    PS4_AUDIO_S32,  /* also 24 bits in 4 bytes */
    PS4_AUDIO_FLOAT,
};

/* at the start of the shared memory; the frames follow at PS4_AUDIO_DATA */
struct ps4_audio_ring
{
    uint32_t magic;
    uint32_t version;
    uint32_t format;        /* enum ps4_audio_format */
    uint32_t rate;          /* frames a second */
    uint32_t channels;
    uint32_t channel_mask;  /* the channels' speakers (WAVEFORMATEXTENSIBLE's dwChannelMask) */
    uint32_t frame_bytes;
    uint32_t ring_frames;
    /* the program's */
    uint64_t written;       /* frames put in so far */
    uint64_t discard;       /* frames before this are not to be played (IAudioClient::Reset) */
    uint32_t running;       /* Start .. Stop: the desktop takes frames only then */
    float volume[PS4_AUDIO_CHANNELS];  /* master * channel * session, 0 when muted */
    char name[64];          /* the program, for the log */
    /* the desktop's */
    uint64_t played;        /* frames taken so far */
    uint32_t underruns;     /* times it was running and had too few */
};

#define PS4_AUDIO_DATA_OFFSET (((uint32_t)sizeof(struct ps4_audio_ring) + 63u) & ~63u)
#define PS4_AUDIO_DATA(ring) ((unsigned char *)(ring) + PS4_AUDIO_DATA_OFFSET)

/* the stream's first message to the desktop, with the shared memory's descriptor */
struct ps4_audio_hello
{
    uint32_t magic;
    uint32_t size;          /* of the shared memory */
};

#endif /* PS4_AUDIO_H */
