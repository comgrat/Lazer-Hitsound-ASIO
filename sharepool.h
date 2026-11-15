#pragma once
#ifdef _WIN32
#include <wtypes.h>
typedef unsigned __int64 QWORD;
#else
#include <stdint.h>
#define WINAPI
#define CALLBACK
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint64_t QWORD;
#ifndef __OBJC__
typedef int BOOL;
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define LOBYTE(a) (BYTE)(a)
#define HIBYTE(a) (BYTE)((a)>>8)
#define LOWORD(a) (WORD)(a)
#define HIWORD(a) (WORD)((a)>>16)
#define MAKEWORD(a,b) (WORD)(((a)&0xff)|((b)<<8))
#define MAKELONG(a,b) (DWORD)(((a)&0xffff)|((b)<<16))
#endif

#define DETAILOUTPUT 0

#define BASS_ATTRIB_FREQ            1
#define BASS_ATTRIB_VOL             2
#define BASS_ATTRIB_PAN             3

#define BASS_SAMPLE_8BITS           1
#define BASS_SAMPLE_LOOP            4
#define BASS_SAMPLE_FLOAT           256
#define BASS_ORIGRES_FLOAT          65536

// BASSmix ABI: 0x2000 is BUFFER, not PAUSE. Keep independently validated.
#define BASS_MIXER_CHAN_PAUSE       0x20000

typedef unsigned __int64 QWORD;
typedef DWORD HSAMPLE;
typedef DWORD HCHANNEL;

const int EVENT_POOL_SIZE = 8192;
const int MAPPING_NAME_LENGTH = 128;
const DWORD ASIO_PROTOCOL_MAGIC = 0x41534932;
const int SAMPLE_ACK_COUNT = 4096;

enum EventType
{
    EVENT_NONE = 0,
    EVENT_SAMPLE_LOADED = 1,
    EVENT_SAMPLE_FREED = 2,
    EVENT_CHANNEL_CREATED = 3,
    EVENT_CHANNEL_UPDATED = 4,
    EVENT_CHANNEL_STATE = 5,
    EVENT_CHANNEL_REMOVED = 6,
};

enum ChannelState
{
    CHANNEL_STATE_PAUSED = 0,
    CHANNEL_STATE_PLAYING = 1,
};

enum ChannelUpdateMask
{
    CHANNEL_UPDATE_VOLUME = 1 << 0,
    CHANNEL_UPDATE_PAN = 1 << 1,
    CHANNEL_UPDATE_FREQUENCY = 1 << 2,
    CHANNEL_UPDATE_LOOP = 1 << 3,
};

enum SampleFormat
{
    SAMPLE_FORMAT_UNKNOWN = 0,
    SAMPLE_FORMAT_PCM8 = 1,
    SAMPLE_FORMAT_PCM16 = 2,
    SAMPLE_FORMAT_PCM24 = 3,
    SAMPLE_FORMAT_PCM32 = 4,
    SAMPLE_FORMAT_FLOAT = 5,
};

struct SampleLoadedPayload
{
    HSAMPLE sample;
    DWORD generation;
    DWORD sampleRate;
    DWORD channels;
    DWORD format;
    DWORD dataLength;
    DWORD flags;
    char mappingName[MAPPING_NAME_LENGTH];
};

struct SampleFreedPayload
{
    HSAMPLE sample;
};

struct ChannelCreatedPayload
{
    HSAMPLE sample;
    HCHANNEL channel;
    DWORD flags;
};

struct ChannelUpdatedPayload
{
    HCHANNEL channel;
    DWORD updateMask;
    float volume;
    float pan;
    float frequency;
    DWORD looping;
};

struct ChannelStatePayload
{
    HCHANNEL channel;
    DWORD state;
    double time;
    HSAMPLE sample;
    float volume;
    float pan;
    float frequency;
    DWORD looping;
};

struct ChannelRemovedPayload
{
    HCHANNEL channel;
};

struct Event
{
    DWORD type;
    DWORD generation;
    union
    {
        SampleLoadedPayload sampleLoaded;
        SampleFreedPayload sampleFreed;
        ChannelCreatedPayload channelCreated;
        ChannelUpdatedPayload channelUpdated;
        ChannelStatePayload channelState;
        ChannelRemovedPayload channelRemoved;
    } data;
};

struct sharepool
{
    DWORD magic;
    DWORD targetPid;
    volatile LONG backendReady;
    volatile LONG heartbeat;
    volatile LONG head;
    volatile LONG tail;
    struct SampleAck
    {
        volatile LONG sample;
        volatile LONG requestedGeneration;
        volatile LONG readyGeneration;
    } sampleAcks[SAMPLE_ACK_COUNT];
    Event events[EVENT_POOL_SIZE];
};
