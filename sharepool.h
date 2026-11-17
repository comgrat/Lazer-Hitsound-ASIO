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
const DWORD ASIO_PROTOCOL_MAGIC = 0x41534936; // Recovery cause and lock diagnostics.
const int SAMPLE_ACK_COUNT = 4096;
const int CHANNEL_FAILURE_COUNT = 256;

enum EventType
{
    EVENT_NONE = 0,
    EVENT_SAMPLE_LOADED = 1,
    EVENT_SAMPLE_FREED = 2,
    EVENT_CHANNEL_CREATED = 3,
    EVENT_CHANNEL_UPDATED = 4,
    EVENT_CHANNEL_STATE = 5,
    EVENT_CHANNEL_REMOVED = 6,
    EVENT_RECOVERY_COMMIT = 7, // FIFO end of the hook's sample rebuild snapshot.
};

// Running -> Requested (either side/DSP) -> BackendStopped (backend)
// -> Replaying (hook) -> Running (backend, after the FIFO commit marker).
// backendReady=0 is permanent and no recovery transition may set it back to 1.
enum RecoveryState : LONG
{
    RECOVERY_RUNNING = 0,
    RECOVERY_REQUESTED = 1,
    RECOVERY_BACKEND_STOPPED = 2,
    RECOVERY_REPLAYING = 3,
    RECOVERY_RECORDING = 4, // CAS winner publishes cause before REQUESTED.
};
enum RecoverySource : LONG { RECOVERY_SOURCE_UNKNOWN=0, RECOVERY_SOURCE_DSP=1, RECOVERY_SOURCE_BACKEND=2, RECOVERY_SOURCE_HOOK=3, RECOVERY_SOURCE_MONITOR=4 };
enum RecoveryStale : LONG { RECOVERY_STALE_NONE=0, RECOVERY_STALE_BACKEND=1, RECOVERY_STALE_HOOK=2 };

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
    DWORD epoch;
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
    LONG startupDebug; // Backend parsed config; immutable for this attachment.
    volatile LONG diagnosticsEnabled; // Debug Log or backend ASIO_DIAGNOSTICS.
    volatile LONG recoveryState;
    volatile LONG epoch; // Advanced by backend only after old voices stop.
    volatile LONG hookHeartbeat;
    volatile LONG hookAttached;
    volatile LONG recoveries;
    volatile LONG recoverySource;
    volatile LONG recoveryStale;
    volatile LONG recoveryAgeMs;
    volatile LONG recoveryEpoch;
    volatile LONG recoveryTick;
    volatile LONG stateLockWaitMaxUs;
    volatile LONG stateLockHoldMaxUs;
    volatile LONG nativeCallMaxUs;
    volatile LONG nativeCallMaxId;
    volatile LONG retiredThroughGeneration; // Hook snapshot fence for delayed producers.
    volatile LONG failureHead;
    volatile LONG failureTail;
    struct ChannelFailure { DWORD channel; DWORD generation; } channelFailures[CHANNEL_FAILURE_COUNT];
    struct SampleAck
    {
        volatile LONG sample;
        volatile LONG requestedGeneration;
        volatile LONG readyGeneration;
    } sampleAcks[SAMPLE_ACK_COUNT];
    Event events[EVENT_POOL_SIZE];
};
