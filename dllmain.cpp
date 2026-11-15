#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <unordered_map>
#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <mutex>

#include "MinHook.h"
#include "sharepool.h"
#include "routing.h"
#include "live_attach.h"
#include "managed_host.h"
#include "async_text_log.h"

using namespace std;

struct BASS_SAMPLE
{
    DWORD Frequency;
    float Volume;
    float Pan;
    DWORD Flags;
    DWORD Length;
    DWORD Max;
    DWORD OrigRes;
    DWORD Chans;
    DWORD Mingap;
    DWORD Mode3D;
    float MinDist;
    float MaxDist;
    DWORD IAngle;
    DWORD OAngle;
    float OutVol;
    DWORD Reserved[2];
};
static_assert(sizeof(BASS_SAMPLE) == 68, "BASS_SAMPLE ABI must match BASS 2.4");

struct SampleInfo
{
    DWORD sampleRate;
    DWORD channels;
    DWORD format;
    DWORD dataLength;
    DWORD flags;
    string mappingName;
    HANDLE mappingHandle;
    DWORD generation = 0;

    SampleInfo()
        : sampleRate(0), channels(0), format(SAMPLE_FORMAT_UNKNOWN), dataLength(0), flags(0), mappingHandle(NULL)
    {
    }
};

struct ChannelInfo
{
    HSAMPLE sample;
    float volume;
    float pan;
    float frequency;
    bool looping;
    bool paused;
    bool eligible = false;
    float sampleGain = 0;
    DWORD generation = 0;
    DWORD incarnation = 0; // Original allocation identity survives only explicit restart epochs.
    asio::RoutingState routing;
    bool restorePending = false;

    ChannelInfo()
        : sample(0), volume(1.0f), pan(0.0f), frequency(0.0f), looping(false), paused(true)
    {
    }
};

typedef HSAMPLE(WINAPI *fpBASS_SampleLoad)(BOOL mem, const void *file, QWORD offset, DWORD length, DWORD max, DWORD flags);
typedef BOOL(WINAPI *fpBASS_SampleGetInfo)(HSAMPLE handle, BASS_SAMPLE *info);
typedef BOOL(WINAPI *fpBASS_SampleGetData)(HSAMPLE handle, void *buffer);
typedef BOOL(WINAPI *fpBASS_SampleFree)(HSAMPLE handle);
typedef HCHANNEL(WINAPI *fpBASS_SampleGetChannel)(HSAMPLE handle, DWORD flags);
typedef BOOL(WINAPI *fpBASS_ChannelPlay)(DWORD handle, BOOL restart);
typedef BOOL(WINAPI *fpBASS_ChannelPause)(DWORD handle);
typedef BOOL(WINAPI *fpBASS_ChannelStop)(DWORD handle);
typedef DWORD(WINAPI *fpBASS_ChannelIsActive)(DWORD handle);
typedef BOOL(WINAPI *fpBASS_ChannelSetAttribute)(DWORD handle, DWORD attrib, float value);
typedef DWORD(WINAPI *fpBASS_ChannelFlags)(DWORD handle, DWORD flags, DWORD mask);
typedef BOOL(WINAPI *fpBASS_StreamFree)(DWORD handle);

typedef BOOL(WINAPI *fpBASS_Mixer_StreamAddChannel)(DWORD handle, DWORD channel, DWORD flags);
typedef DWORD(WINAPI *fpBASS_Mixer_ChannelFlags)(DWORD handle, DWORD flags, DWORD mask);
typedef BOOL(WINAPI *fpBASS_Mixer_ChannelRemove)(DWORD handle);

static fpBASS_SampleLoad pOrigBASS_SampleLoad = NULL;
static fpBASS_SampleGetInfo pOrigBASS_SampleGetInfo = NULL;
static fpBASS_SampleGetData pOrigBASS_SampleGetData = NULL;
static fpBASS_SampleFree pOrigBASS_SampleFree = NULL;
static fpBASS_SampleGetChannel pOrigBASS_SampleGetChannel = NULL;
static fpBASS_ChannelPlay pOrigBASS_ChannelPlay = NULL;
static fpBASS_ChannelPause pOrigBASS_ChannelPause = NULL;
static fpBASS_ChannelStop pOrigBASS_ChannelStop = NULL;
static fpBASS_ChannelIsActive pOrigBASS_ChannelIsActive = NULL;
static fpBASS_ChannelSetAttribute pOrigBASS_ChannelSetAttribute = NULL;
static fpBASS_ChannelFlags pOrigBASS_ChannelFlags = NULL;
static fpBASS_StreamFree pOrigBASS_StreamFree = NULL;
static fpBASS_Mixer_StreamAddChannel pOrigBASS_Mixer_StreamAddChannel = NULL;
static fpBASS_Mixer_ChannelFlags pOrigBASS_Mixer_ChannelFlags = NULL;
static fpBASS_Mixer_ChannelRemove pOrigBASS_Mixer_ChannelRemove = NULL;

static struct sharepool *MyPool = NULL;
static HANDLE hMapFile = NULL;
static LPVOID lpBase = NULL;
static CRITICAL_SECTION queueLock;
static CRITICAL_SECTION stateLock;
static CRITICAL_SECTION logLock;
static std::atomic<bool> hooksInitialised{false};
static std::atomic<bool> liveManagedReady{true};
static bool locksInitialised = false;
static bool logLockInitialised = false;
static std::atomic<bool> pollerRunning{false};
static LONG nextGeneration = 0;
static std::recursive_mutex samplePublishMutex;
static HMODULE selfModule = NULL;
static char logFilePath[MAX_PATH] = {0};
static char configFilePath[MAX_PATH] = {0};
static int debugLogEnabled = 0;
static asio::AsyncTextLog* hookTextLog = nullptr;
static std::atomic<bool> traceEnabled{false};
// Optional lock-free diagnostic counters, read by the managed timer.
static std::atomic<DWORD> traceCounters[12]{};
static void traceCount(unsigned index)
{
    if (traceEnabled) traceCounters[index].fetch_add(1, std::memory_order_relaxed);
}
extern "C" __declspec(dllexport) DWORD AsioBridgeTrace(DWORD index)
{
    return index < 12 ? traceCounters[index].load(std::memory_order_relaxed) : 0;
}

static unordered_map<HSAMPLE, SampleInfo> sampleInfoMap;
static unordered_map<HCHANNEL, ChannelInfo> channelInfoMap;

static bool publishChannelState(HCHANNEL channel, DWORD state);
static void logPrintf(const char *format, ...);

static void restoreNativeVolume(HCHANNEL channel, ChannelInfo &info)
{
    bool restored = pOrigBASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, info.volume) != FALSE;
    if (!restored && !info.restorePending)
        logPrintf("[ASIO Hook] native volume restore failed channel=%lu; retrying\n", (unsigned long)channel);
    info.restorePending = !restored;
    if (!restored && MyPool) InterlockedExchange(&MyPool->backendReady, 0);
}

static sharepool::SampleAck *findSampleAck(HSAMPLE sample, bool allocate = false)
{
    if (!MyPool) return nullptr;
    sharepool::SampleAck *empty = nullptr;
    for (int probe = 0; probe < SAMPLE_ACK_COUNT; ++probe)
    {
        auto &ack = MyPool->sampleAcks[(sample + probe) % SAMPLE_ACK_COUNT];
        if (DWORD(ack.sample) == sample) return &ack;
        if (!ack.sample && !empty) empty = &ack;
    }
    if (allocate && empty) { InterlockedExchange(&empty->sample, LONG(sample)); return empty; }
    return nullptr;
}

static bool backendAlive()
{
    return MyPool != NULL && MyPool->magic == ASIO_PROTOCOL_MAGIC &&
        MyPool->targetPid == GetCurrentProcessId() && MyPool->backendReady != 0 &&
        DWORD(GetTickCount() - DWORD(MyPool->heartbeat)) < 250;
}

static bool sampleReady(HSAMPLE sample)
{
    auto it = sampleInfoMap.find(sample);
    if (it == sampleInfoMap.end() || MyPool == NULL)
        return false;
    const auto *ack = findSampleAck(sample);
    return ack && DWORD(ack->readyGeneration) == it->second.generation;
}

static bool queueAvailable()
{
    return MyPool != NULL && (MyPool->tail + 1) % EVENT_POOL_SIZE != MyPool->head;
}

static DWORD channelGeneration(HCHANNEL channel)
{
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    DWORD generation = it != channelInfoMap.end() ? it->second.generation : 0;
    LeaveCriticalSection(&stateLock);
    return generation;
}

static void initLogPath()
{
    if (logFilePath[0] != '\0')
        return;

    char modulePath[MAX_PATH] = {0};
    DWORD length = GetModuleFileNameA(selfModule, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;

    char *slash = strrchr(modulePath, '\\');
    if (slash == NULL)
        slash = strrchr(modulePath, '/');
    if (slash != NULL)
        *(slash + 1) = '\0';
    else
        modulePath[0] = '\0';

    snprintf(logFilePath, MAX_PATH, "%sasio_hook.log", modulePath);
    snprintf(configFilePath, MAX_PATH, "%sconfig.ini", modulePath);
}

static int readValue(FILE *fp, int defaultValue)
{
    int x = 0, f = 1;
    int raw = fgetc(fp);
    if (raw == EOF)
        return defaultValue;
    char ch = (char)raw;
    while (ch < '0' || ch > '9')
    {
        if (ch == '-')
            f = -1;
        raw = fgetc(fp);
        if (raw == EOF)
            return defaultValue;
        ch = (char)raw;
    }
    while (ch >= '0' && ch <= '9')
    {
        x = x * 10 + ch - '0';
        raw = fgetc(fp);
        if (raw == EOF)
            break;
        ch = (char)raw;
    }
    return x * f;
}

static void initDebugLogSetting()
{
    initLogPath();
    if (configFilePath[0] == '\0')
        return;

    FILE *fp = fopen(configFilePath, "r");
    if (fp == NULL)
        return;

    int discard = 0;
    discard = readValue(fp, 128);
    discard = readValue(fp, 0);
    discard = readValue(fp, 48000);
    discard = readValue(fp, 100);
    debugLogEnabled = readValue(fp, debugLogEnabled) ? 1 : 0;
    (void)discard;
    fclose(fp);
}

static void logPrintf(const char *format, ...)
{
    if (!logLockInitialised)
        return;

    if (!debugLogEnabled)
        return;

    initLogPath();
    if (logFilePath[0] == '\0')
        return;

    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (hookTextLog) hookTextLog->write("%s",buffer);
}

static double CPUclock()
{
    LARGE_INTEGER nFreq;
    LARGE_INTEGER t1;
    QueryPerformanceFrequency(&nFreq);
    QueryPerformanceCounter(&t1);
    return t1.QuadPart * 1000.0 / nFreq.QuadPart;
}

static bool pushEvent(const Event &event)
{
    if (MyPool == NULL || !MyPool->backendReady)
        return false;

    EnterCriticalSection(&queueLock);

    LONG next = (MyPool->tail + 1) % EVENT_POOL_SIZE;
    if (next == MyPool->head)
    {
        InterlockedExchange(&MyPool->backendReady, 0);
        LeaveCriticalSection(&queueLock);
        logPrintf("[ASIO Hook] Event queue full, dropping type=%lu\n", (unsigned long)event.type);
        return false;
    }

    MyPool->events[MyPool->tail] = event;
    MemoryBarrier();
    MyPool->tail = next;

    LeaveCriticalSection(&queueLock);
    return true;
}

static DWORD sampleFormatFromInfo(const BASS_SAMPLE &info)
{
    if (info.Flags & BASS_SAMPLE_FLOAT)
        return SAMPLE_FORMAT_FLOAT;

    if (info.Flags & BASS_SAMPLE_8BITS)
        return SAMPLE_FORMAT_PCM8;

    // BASS samples are 16-bit PCM unless explicitly flagged as 8-bit or float.
    return SAMPLE_FORMAT_PCM16;
}

static DWORD bytesPerSample(DWORD format)
{
    switch (format)
    {
    case SAMPLE_FORMAT_PCM8:
        return 1;
    case SAMPLE_FORMAT_PCM16:
        return 2;
    case SAMPLE_FORMAT_PCM24:
        return 3;
    case SAMPLE_FORMAT_PCM32:
    case SAMPLE_FORMAT_FLOAT:
        return 4;
    default:
        return 0;
    }
}

static bool publishSample(HSAMPLE sample)
{
    std::lock_guard<std::recursive_mutex> publishGuard(samplePublishMutex);
    EnterCriticalSection(&stateLock);
    bool existingSample = sampleInfoMap.find(sample) != sampleInfoMap.end();
    LeaveCriticalSection(&stateLock);
    if (existingSample) return true;
    if (!backendAlive()) return false;
    if (sample == 0 || pOrigBASS_SampleGetInfo == NULL || pOrigBASS_SampleGetData == NULL)
    {
        logPrintf("[ASIO Hook] publishSample skipped sample=%lu reason=invalid_state getInfo=%d getData=%d\n",
                  (unsigned long)sample,
                  pOrigBASS_SampleGetInfo != NULL,
                  pOrigBASS_SampleGetData != NULL);
        return false;
    }

    BASS_SAMPLE info;
    memset(&info, 0, sizeof(info));
    if (!pOrigBASS_SampleGetInfo(sample, &info))
    {
        logPrintf("[ASIO Hook] publishSample failed sample=%lu step=get_info\n", (unsigned long)sample);
        return false;
    }

    DWORD format = sampleFormatFromInfo(info);
    DWORD bytes = bytesPerSample(format);
    if (format == SAMPLE_FORMAT_UNKNOWN || bytes == 0 || info.Length == 0 || info.Chans == 0 || info.Frequency == 0)
    {
        logPrintf("[ASIO Hook] publishSample failed sample=%lu step=validate format=%lu bytes=%lu length=%lu chans=%lu freq=%lu flags=%lu origRes=%lu\n",
                  (unsigned long)sample,
                  (unsigned long)format,
                  (unsigned long)bytes,
                  (unsigned long)info.Length,
                  (unsigned long)info.Chans,
                  (unsigned long)info.Frequency,
                  (unsigned long)info.Flags,
                  (unsigned long)info.OrigRes);
        return false;
    }

    char mappingName[MAPPING_NAME_LENGTH];
    DWORD generation = DWORD(InterlockedIncrement(&nextGeneration));
    sprintf(mappingName, "Local\\OsuAsioSample_%lu_%lu_%lu", GetCurrentProcessId(), (unsigned long)sample, (unsigned long)generation);

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, info.Length, mappingName);
    if (mapping == NULL)
    {
        logPrintf("[ASIO Hook] publishSample failed sample=%lu step=create_mapping length=%lu error=%lu\n",
                  (unsigned long)sample,
                  (unsigned long)info.Length,
                  (unsigned long)GetLastError());
        return false;
    }

    void *mappingView = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, info.Length);
    if (mappingView == NULL)
    {
        logPrintf("[ASIO Hook] publishSample failed sample=%lu step=map_view length=%lu error=%lu\n",
                  (unsigned long)sample,
                  (unsigned long)info.Length,
                  (unsigned long)GetLastError());
        CloseHandle(mapping);
        return false;
    }

    bool dataOk = pOrigBASS_SampleGetData(sample, mappingView) != FALSE;
    UnmapViewOfFile(mappingView);

    if (!dataOk)
    {
        logPrintf("[ASIO Hook] publishSample failed sample=%lu step=get_data length=%lu\n",
                  (unsigned long)sample,
                  (unsigned long)info.Length);
        CloseHandle(mapping);
        return false;
    }

    SampleInfo sampleInfo;
    sampleInfo.sampleRate = info.Frequency;
    sampleInfo.channels = info.Chans;
    sampleInfo.format = format;
    sampleInfo.dataLength = info.Length;
    sampleInfo.flags = info.Flags;
    sampleInfo.mappingName = mappingName;
    sampleInfo.mappingHandle = mapping;
    sampleInfo.generation = generation;

    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_SAMPLE_LOADED;
    event.data.sampleLoaded.sample = sample;
    event.data.sampleLoaded.generation = generation;
    event.data.sampleLoaded.sampleRate = sampleInfo.sampleRate;
    event.data.sampleLoaded.channels = sampleInfo.channels;
    event.data.sampleLoaded.format = sampleInfo.format;
    event.data.sampleLoaded.dataLength = sampleInfo.dataLength;
    event.data.sampleLoaded.flags = sampleInfo.flags;
    strncpy(event.data.sampleLoaded.mappingName, mappingName, MAPPING_NAME_LENGTH - 1);
    event.data.sampleLoaded.mappingName[MAPPING_NAME_LENGTH - 1] = '\0';

    bool hadExisting = false;
    SampleInfo previousSampleInfo;
    EnterCriticalSection(&stateLock);
    auto existing = sampleInfoMap.find(sample);
    if (existing != sampleInfoMap.end())
    {
        previousSampleInfo = existing->second;
        hadExisting = true;
    }
    sampleInfoMap[sample] = sampleInfo;
    if (MyPool != NULL)
    {
        auto *ack = findSampleAck(sample, true);
        if (ack)
        {
            InterlockedExchange(&ack->readyGeneration, 0);
            InterlockedExchange(&ack->requestedGeneration, LONG(generation));
        }
    }
    LeaveCriticalSection(&stateLock);

    if (pushEvent(event))
    {
        logPrintf("[ASIO Hook] publishSample success sample=%lu length=%lu chans=%lu freq=%lu format=%lu flags=%lu\n",
                  (unsigned long)sample,
                  (unsigned long)sampleInfo.dataLength,
                  (unsigned long)sampleInfo.channels,
                  (unsigned long)sampleInfo.sampleRate,
                  (unsigned long)sampleInfo.format,
                  (unsigned long)sampleInfo.flags);
        if (hadExisting && previousSampleInfo.mappingHandle != NULL)
            CloseHandle(previousSampleInfo.mappingHandle);
        return true;
    }

    EnterCriticalSection(&stateLock);
    auto it = sampleInfoMap.find(sample);
    if (it != sampleInfoMap.end() && it->second.mappingHandle == mapping)
    {
        sampleInfoMap.erase(it);
        if (hadExisting)
            sampleInfoMap[sample] = previousSampleInfo;
    }
    LeaveCriticalSection(&stateLock);

    CloseHandle(mapping);
    logPrintf("[ASIO Hook] publishSample failed sample=%lu step=push_event\n", (unsigned long)sample);
    return false;
}

static void ensureSamplePublished(HSAMPLE sample)
{
    if (sample == 0)
        return;

    EnterCriticalSection(&stateLock);
    bool alreadyPublished = sampleInfoMap.find(sample) != sampleInfoMap.end();
    LeaveCriticalSection(&stateLock);

    if (alreadyPublished)
    {
        logPrintf("[ASIO Hook] ensureSamplePublished skipped sample=%lu reason=already_published\n", (unsigned long)sample);
        return;
    }

    logPrintf("[ASIO Hook] ensureSamplePublished retry sample=%lu\n", (unsigned long)sample);
    if (!publishSample(sample))
        logPrintf("[ASIO Hook] ensureSamplePublished retry_failed sample=%lu\n", (unsigned long)sample);
}

static DWORD WINAPI monitorChannels(LPVOID)
{
    const DWORD BASS_ACTIVE_PLAYING = 1;
    const DWORD BASS_ACTIVE_STALLED = 2;

    while (pollerRunning)
    {
        EnterCriticalSection(&stateLock);
        for (auto &entry : channelInfoMap)
        {
            if (entry.second.restorePending)
                restoreNativeVolume(entry.first, entry.second);
            if (entry.second.routing.suppressNative() && !backendAlive())
            {
                publishChannelState(entry.first, CHANNEL_STATE_PAUSED);
                InterlockedExchange(&MyPool->backendReady, 0);
                entry.second.routing.failover();
                restoreNativeVolume(entry.first, entry.second);
            }
            if (!entry.second.routing.playing)
                continue;
            DWORD active = pOrigBASS_ChannelIsActive != NULL ? pOrigBASS_ChannelIsActive(entry.first) : 0;
            bool shouldPlay = active == BASS_ACTIVE_PLAYING || active == BASS_ACTIVE_STALLED;

            if (!shouldPlay && !entry.second.paused)
            {
                entry.second.paused = true;
                entry.second.routing.pause();
                publishChannelState(entry.first, CHANNEL_STATE_PAUSED);
            }
        }
        LeaveCriticalSection(&stateLock);

        Sleep(1);
    }

    return 0;
}

static void publishSampleFreed(HSAMPLE sample)
{
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_SAMPLE_FREED;
    event.data.sampleFreed.sample = sample;
    pushEvent(event);
}

static void publishChannelCreated(HSAMPLE sample, HCHANNEL channel, DWORD flags)
{
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_CHANNEL_CREATED;
    event.generation = channelGeneration(channel);
    event.data.channelCreated.sample = sample;
    event.data.channelCreated.channel = channel;
    event.data.channelCreated.flags = flags;
    pushEvent(event);
}

static void publishChannelUpdated(HCHANNEL channel, DWORD mask, const ChannelInfo &info)
{
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_CHANNEL_UPDATED;
    event.generation = info.generation;
    event.data.channelUpdated.channel = channel;
    event.data.channelUpdated.updateMask = mask;
    event.data.channelUpdated.volume = info.sampleGain;
    event.data.channelUpdated.pan = info.pan;
    event.data.channelUpdated.frequency = info.frequency;
    event.data.channelUpdated.looping = info.looping ? 1 : 0;
    pushEvent(event);
}

static bool publishChannelState(HCHANNEL channel, DWORD state)
{
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_CHANNEL_STATE;
    event.generation = channelGeneration(channel);
    event.data.channelState.channel = channel;
    event.data.channelState.state = state;
    event.data.channelState.time = DETAILOUTPUT ? CPUclock() : 0;
    EnterCriticalSection(&stateLock);
    auto current = channelInfoMap.find(channel);
    if (current != channelInfoMap.end())
    {
        event.generation = current->second.generation;
        event.data.channelState.sample = current->second.sample;
        event.data.channelState.volume = current->second.sampleGain;
        event.data.channelState.pan = current->second.pan;
        event.data.channelState.frequency = current->second.frequency;
        event.data.channelState.looping = current->second.looping;
    }
    bool accepted = pushEvent(event);
    if (accepted && state == CHANNEL_STATE_PLAYING) traceCount(11);
    if (accepted)
        logPrintf("{\"event\":\"%s\",\"channel\":%lu,\"generation\":%lu,\"route\":\"external\",\"gain\":%.6f,\"source\":\"hook_queue\",\"tick_ms\":%llu,\"sample\":%lu}\n",
            state == CHANNEL_STATE_PLAYING ? "start" : "pause", (unsigned long)channel,
            (unsigned long)event.generation, event.data.channelState.volume, GetTickCount64(),
            (unsigned long)event.data.channelState.sample);
    if (!accepted && current != channelInfoMap.end())
    {
        current->second.routing.failover();
        restoreNativeVolume(channel, current->second);
    }
    LeaveCriticalSection(&stateLock);
    return accepted;
}

static void publishChannelRemoved(HCHANNEL channel)
{
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_CHANNEL_REMOVED;
    event.generation = channelGeneration(channel);
    event.data.channelRemoved.channel = channel;
    pushEvent(event);
}

// Caller holds stateLock. Never infer playback from a decoding channel's active state.
static void startExternal(HCHANNEL channel, ChannelInfo &info)
{
    bool alive = backendAlive(), ready = sampleReady(info.sample), queue = queueAvailable();
    traceCount(4);
    if (!info.eligible) traceCount(5);
    if (!alive) traceCount(6);
    if (!ready) traceCount(7);
    if (!queue) traceCount(8);
    if (info.routing.route == asio::Route::Native) traceCount(9);
    if (info.routing.start(info.eligible, alive, ready, queue))
    {
        // Mute only the identified channel; the native mixer still advances its timeline.
        if (!pOrigBASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, 0) ||
            !publishChannelState(channel, CHANNEL_STATE_PLAYING))
        {
            info.routing.failover();
            restoreNativeVolume(channel, info);
        }
    }
    if (!info.routing.suppressNative())
        restoreNativeVolume(channel, info);
    info.paused = false;
}

static bool muteBeforeStart(HCHANNEL channel)
{
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    bool mute = it != channelInfoMap.end() && (it->second.routing.suppressNative() ||
        (it->second.routing.route == asio::Route::Undecided && it->second.eligible &&
         backendAlive() && sampleReady(it->second.sample) && queueAvailable()));
    if (mute)
        mute = pOrigBASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, 0) != FALSE;
    LeaveCriticalSection(&stateLock);
    return mute;
}

static void restoreIfNative(HCHANNEL channel)
{
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    if (it != channelInfoMap.end() && !it->second.routing.suppressNative())
        restoreNativeVolume(channel, it->second);
    LeaveCriticalSection(&stateLock);
}

extern "C" __declspec(dllexport) BOOL AsioBridgeReady()
{
    return hooksInitialised && liveManagedReady && backendAlive();
}

static void AsioChannelContext(DWORD channel, float sampleGain, DWORD eligible)
{
    if (!hooksInitialised)
        return;
    if (channel == 0 && !eligible)
    {
        if (MyPool) InterlockedExchange(&MyPool->backendReady, 0);
        return;
    }
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    if (it != channelInfoMap.end())
    {
        auto &info = it->second;
        info.eligible = eligible && std::isfinite(sampleGain) && sampleGain >= 0 && sampleGain <= 1;
        info.sampleGain = info.eligible ? sampleGain : 0;
        if (!info.eligible && info.routing.suppressNative())
        {
            publishChannelState(channel, CHANNEL_STATE_PAUSED);
            info.routing.failover();
            restoreNativeVolume(channel, info);
        }
        publishChannelUpdated(channel, CHANNEL_UPDATE_VOLUME, info);
    }
    LeaveCriticalSection(&stateLock);
}

extern "C" __declspec(dllexport) DWORD AsioChannelGeneration(DWORD channel)
{
    return hooksInitialised ? channelGeneration(channel) : 0;
}

extern "C" __declspec(dllexport) DWORD AsioChannelPlaybackGeneration(DWORD channel, DWORD allocation)
{
    if (!hooksInitialised) return 0;
    EnterCriticalSection(&stateLock);
    auto it=channelInfoMap.find(channel);
    DWORD result=it!=channelInfoMap.end() && it->second.incarnation==allocation ? it->second.generation : 0;
    LeaveCriticalSection(&stateLock);
    return result;
}

extern "C" __declspec(dllexport) void AsioChannelContextV2(DWORD channel, DWORD generation, float sampleGain, DWORD eligible)
{
    if (!hooksInitialised) return;
    if (!channel) { AsioChannelContext(0, 0, 0); return; }
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    if (it != channelInfoMap.end() && it->second.generation == generation)
    {
        traceCount(0);
        if (eligible) traceCount(1);
        AsioChannelContext(channel, sampleGain, eligible);
        logPrintf("{\"event\":\"context\",\"channel\":%lu,\"generation\":%lu,\"eligible\":%lu,\"gain\":%.6f}\n",
            (unsigned long)channel, (unsigned long)generation, (unsigned long)eligible, sampleGain);
    }
    else traceCount(2);
    LeaveCriticalSection(&stateLock);
}

static bool waitForModule(const char *moduleName, HMODULE *module)
{
    for (int i = 0; i < 200; ++i)
    {
        *module = GetModuleHandleA(moduleName);
        if (*module != NULL)
            return true;
        Sleep(50);
    }

    return false;
}

static FARPROC requireProc(HMODULE module, const char *name)
{
    FARPROC proc = GetProcAddress(module, name);
    if (proc == NULL)
    {
        char message[256];
        sprintf(message, "Missing export: %s", name);
        logPrintf("[ASIO Hook] %s\n", message);
    }

    return proc;
}

HSAMPLE WINAPI MyBASS_SampleLoad(BOOL mem, const void *file, QWORD offset, DWORD length, DWORD max, DWORD flags)
{
    std::lock_guard<std::recursive_mutex> publishGuard(samplePublishMutex);
    HSAMPLE sample = pOrigBASS_SampleLoad(mem, file, offset, length, max, flags);
    logPrintf("[ASIO Hook] BASS_SampleLoad mem=%d offset=%llu length=%lu max=%lu flags=%lu -> sample=%lu\n",
              mem ? 1 : 0,
              (unsigned long long)offset,
              (unsigned long)length,
              (unsigned long)max,
              (unsigned long)flags,
              (unsigned long)sample);
    if (sample != 0)
        publishSample(sample);
    return sample;
}

BOOL WINAPI MyBASS_SampleFree(HSAMPLE handle)
{
    std::lock_guard<std::recursive_mutex> publishGuard(samplePublishMutex);
    BOOL result = pOrigBASS_SampleFree(handle);
    if (!result)
        return result;
    logPrintf("[ASIO Hook] BASS_SampleFree sample=%lu\n", (unsigned long)handle);
    publishSampleFreed(handle);

    EnterCriticalSection(&stateLock);
    auto sampleIt = sampleInfoMap.find(handle);
    if (sampleIt != sampleInfoMap.end())
    {
        auto *ack = findSampleAck(handle);
        if (ack && DWORD(ack->requestedGeneration) == sampleIt->second.generation)
        {
            InterlockedExchange(&ack->readyGeneration, 0);
            InterlockedExchange(&ack->requestedGeneration, 0);
            InterlockedExchange(&ack->sample, 0);
        }
        if (sampleIt->second.mappingHandle != NULL)
            CloseHandle(sampleIt->second.mappingHandle);
        sampleInfoMap.erase(sampleIt);
    }

    for (auto it = channelInfoMap.begin(); it != channelInfoMap.end();)
    {
        if (it->second.sample == handle)
            it = channelInfoMap.erase(it);
        else
            ++it;
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

HCHANNEL WINAPI MyBASS_SampleGetChannel(HSAMPLE handle, DWORD flags)
{
    logPrintf("[ASIO Hook] BASS_SampleGetChannel sample=%lu flags=%lu\n",
              (unsigned long)handle,
              (unsigned long)flags);
    ensureSamplePublished(handle);

    HCHANNEL channel = pOrigBASS_SampleGetChannel(handle, flags);
    logPrintf("[ASIO Hook] BASS_SampleGetChannel result sample=%lu channel=%lu\n",
              (unsigned long)handle,
              (unsigned long)channel);
    if (channel == 0)
        return channel;

    EnterCriticalSection(&stateLock);
    ChannelInfo info;
    info.sample = handle;
    info.looping = (flags & BASS_SAMPLE_LOOP) != 0;
    info.paused = true;
    info.generation = DWORD(InterlockedIncrement(&nextGeneration));
    info.incarnation = info.generation;
    channelInfoMap[channel] = info;
    LeaveCriticalSection(&stateLock);

    publishChannelCreated(handle, channel, flags);
    publishChannelUpdated(channel, CHANNEL_UPDATE_LOOP, info);
    return channel;
}

BOOL WINAPI MyBASS_ChannelPlay(DWORD handle, BOOL restart)
{
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(handle);
    DWORD active = it!=channelInfoMap.end() && pOrigBASS_ChannelIsActive ? pOrigBASS_ChannelIsActive(handle) : 0;
    bool newPlayback = it!=channelInfoMap.end() && (restart || active==0);
    muteBeforeStart(handle);
    BOOL result = pOrigBASS_ChannelPlay(handle, restart);
    if (!result) { restoreIfNative(handle); LeaveCriticalSection(&stateLock); return result; }
    if (it != channelInfoMap.end())
    {
        // A confirmed restart is a new playback lifetime, unlike a stale PLAY
        // message for an ended voice. Keep retirement protection in the backend.
        if(newPlayback && it->second.routing.suppressNative()) {
            publishChannelRemoved(handle);
            it->second.generation=DWORD(InterlockedIncrement(&nextGeneration));
            it->second.routing.pause();
            publishChannelCreated(it->second.sample,handle,it->second.looping?BASS_SAMPLE_LOOP:0);
            publishChannelUpdated(handle,CHANNEL_UPDATE_VOLUME|CHANNEL_UPDATE_PAN|CHANNEL_UPDATE_FREQUENCY|CHANNEL_UPDATE_LOOP,it->second);
        }
        startExternal(handle, it->second);
    }
    LeaveCriticalSection(&stateLock);
    return result;
}

BOOL WINAPI MyBASS_ChannelPause(DWORD handle)
{
    EnterCriticalSection(&stateLock);
    BOOL result = pOrigBASS_ChannelPause(handle);
    auto it = channelInfoMap.find(handle);
    if (result && it != channelInfoMap.end())
    {
        bool changed = !it->second.paused || it->second.routing.playing;
        it->second.paused = true;
        it->second.routing.pause();
        if (changed && it->second.routing.suppressNative())
            publishChannelState((HCHANNEL)handle, CHANNEL_STATE_PAUSED);
    }
    LeaveCriticalSection(&stateLock);
    return result;
}

BOOL WINAPI MyBASS_ChannelStop(DWORD handle)
{
    BOOL result = pOrigBASS_ChannelStop(handle);
    if (!result)
        return result;

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(handle);
    if (it != channelInfoMap.end())
    {
        publishChannelRemoved(handle);
        it->second.routing = asio::RoutingState{};
        it->second.eligible = false;
        it->second.paused = true;
        it->second.generation = DWORD(InterlockedIncrement(&nextGeneration));
        it->second.incarnation = it->second.generation;
        publishChannelCreated(it->second.sample, handle, it->second.looping ? BASS_SAMPLE_LOOP : 0);
        publishChannelUpdated(handle, CHANNEL_UPDATE_VOLUME | CHANNEL_UPDATE_PAN | CHANNEL_UPDATE_FREQUENCY | CHANNEL_UPDATE_LOOP, it->second);
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

BOOL WINAPI MyBASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value)
{
    EnterCriticalSection(&stateLock);
    auto before = channelInfoMap.find(handle);
    float nativeValue = attrib == BASS_ATTRIB_VOL && before != channelInfoMap.end() && before->second.routing.suppressNative() ? 0 : value;
    BOOL result = pOrigBASS_ChannelSetAttribute(handle, attrib, nativeValue);
    if (!result)
    {
        LeaveCriticalSection(&stateLock);
        return result;
    }

    auto it = channelInfoMap.find(handle);
    if (it != channelInfoMap.end())
    {
        DWORD mask = 0;
        switch (attrib)
        {
        case BASS_ATTRIB_VOL:
            it->second.volume = value;
            mask = CHANNEL_UPDATE_VOLUME;
            break;
        case BASS_ATTRIB_PAN:
            it->second.pan = value;
            mask = CHANNEL_UPDATE_PAN;
            break;
        case BASS_ATTRIB_FREQ:
            it->second.frequency = value;
            mask = CHANNEL_UPDATE_FREQUENCY;
            break;
        default:
            break;
        }

        if (mask != 0)
            publishChannelUpdated((HCHANNEL)handle, mask, it->second);
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

DWORD WINAPI MyBASS_ChannelFlags(DWORD handle, DWORD flags, DWORD mask)
{
    DWORD result = pOrigBASS_ChannelFlags(handle, flags, mask);
    if (result == DWORD(-1))
        return result;

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(handle);
    if (it != channelInfoMap.end() && (mask & BASS_SAMPLE_LOOP) != 0)
    {
        it->second.looping = (result & BASS_SAMPLE_LOOP) != 0;
        publishChannelUpdated((HCHANNEL)handle, CHANNEL_UPDATE_LOOP, it->second);
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

BOOL WINAPI MyBASS_Mixer_StreamAddChannel(DWORD handle, DWORD channel, DWORD flags)
{
    traceCount(3);
    if (!(flags & BASS_MIXER_CHAN_PAUSE))
        muteBeforeStart(channel);
    BOOL result = pOrigBASS_Mixer_StreamAddChannel(handle, channel, flags);
    if (!result)
    {
        restoreIfNative(channel);
        return result;
    }

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    if (it != channelInfoMap.end())
    {
        it->second.paused = (flags & BASS_MIXER_CHAN_PAUSE) != 0;
        if (!it->second.paused)
            startExternal(channel, it->second);
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

DWORD WINAPI MyBASS_Mixer_ChannelFlags(DWORD handle, DWORD flags, DWORD mask)
{
    if ((mask & BASS_MIXER_CHAN_PAUSE) && !(flags & BASS_MIXER_CHAN_PAUSE)) traceCount(10);
    if ((mask & BASS_MIXER_CHAN_PAUSE) && !(flags & BASS_MIXER_CHAN_PAUSE))
        muteBeforeStart(handle);
    DWORD result = pOrigBASS_Mixer_ChannelFlags(handle, flags, mask);
    if (result == DWORD(-1))
    {
        restoreIfNative(handle);
        return result;
    }

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(handle);
    if (it != channelInfoMap.end() && (mask & BASS_MIXER_CHAN_PAUSE) != 0)
    {
        bool paused = (result & BASS_MIXER_CHAN_PAUSE) != 0;
        if (paused != it->second.paused)
        {
            it->second.paused = paused;
            if (!paused)
                startExternal(handle, it->second);
            else
            {
                it->second.routing.pause();
                if (it->second.routing.suppressNative())
                    publishChannelState(handle, CHANNEL_STATE_PAUSED);
            }
        }
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

BOOL WINAPI MyBASS_Mixer_ChannelRemove(DWORD handle)
{
    EnterCriticalSection(&stateLock);
    BOOL result = pOrigBASS_Mixer_ChannelRemove(handle);
    auto it = channelInfoMap.find(handle);
    if (result && it != channelInfoMap.end())
    {
        bool changed = !it->second.paused || it->second.routing.playing;
        it->second.paused = true;
        it->second.routing.pause();
        if (changed && it->second.routing.suppressNative())
            publishChannelState(handle, CHANNEL_STATE_PAUSED);
    }
    LeaveCriticalSection(&stateLock);
    return result;
}

BOOL WINAPI MyBASS_StreamFree(DWORD handle)
{
    BOOL result = pOrigBASS_StreamFree(handle);
    if (!result)
        return result;
    publishChannelRemoved((HCHANNEL)handle);

    EnterCriticalSection(&stateLock);
    channelInfoMap.erase(handle);
    LeaveCriticalSection(&stateLock);

    return result;
}

extern "C" __declspec(dllexport) void initDLL()
{
    char traceSetting[2] = {};
    traceEnabled = GetEnvironmentVariableA("ASIO_DIAGNOSTICS", traceSetting, sizeof(traceSetting)) == 1 && traceSetting[0] == '1';
    HMODULE bassModule = NULL;
    HMODULE bassMixModule = NULL;

    if (!waitForModule("bass.dll", &bassModule) || !waitForModule("bassmix.dll", &bassMixModule))
    {
        logPrintf("[ASIO Hook] bass.dll or bassmix.dll was not loaded in time\n");
        return;
    }

    InitializeCriticalSection(&queueLock);
    InitializeCriticalSection(&stateLock);
    locksInitialised = true;

    char poolName[128];
    snprintf(poolName, sizeof(poolName), "Local\\OsuAsioV2_%lu", GetCurrentProcessId());
    hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, poolName);
    lpBase = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!(hMapFile && lpBase))
    {
        logPrintf("[ASIO Hook] ShareMemory Failed\n");
        return;
    }

    MyPool = (struct sharepool *)lpBase;
    if (MyPool->magic != ASIO_PROTOCOL_MAGIC || MyPool->targetPid != GetCurrentProcessId())
        return;

    if (MH_Initialize() != MH_OK)
    {
        logPrintf("[ASIO Hook] MinHook initialize failed\n");
        return;
    }

    pOrigBASS_SampleLoad = (fpBASS_SampleLoad)requireProc(bassModule, "BASS_SampleLoad");
    pOrigBASS_SampleGetInfo = (fpBASS_SampleGetInfo)requireProc(bassModule, "BASS_SampleGetInfo");
    pOrigBASS_SampleGetData = (fpBASS_SampleGetData)requireProc(bassModule, "BASS_SampleGetData");
    pOrigBASS_SampleFree = (fpBASS_SampleFree)requireProc(bassModule, "BASS_SampleFree");
    pOrigBASS_SampleGetChannel = (fpBASS_SampleGetChannel)requireProc(bassModule, "BASS_SampleGetChannel");
    pOrigBASS_ChannelPlay = (fpBASS_ChannelPlay)requireProc(bassModule, "BASS_ChannelPlay");
    pOrigBASS_ChannelPause = (fpBASS_ChannelPause)requireProc(bassModule, "BASS_ChannelPause");
    pOrigBASS_ChannelStop = (fpBASS_ChannelStop)requireProc(bassModule, "BASS_ChannelStop");
    pOrigBASS_ChannelIsActive = (fpBASS_ChannelIsActive)requireProc(bassModule, "BASS_ChannelIsActive");
    pOrigBASS_ChannelSetAttribute = (fpBASS_ChannelSetAttribute)requireProc(bassModule, "BASS_ChannelSetAttribute");
    pOrigBASS_ChannelFlags = (fpBASS_ChannelFlags)requireProc(bassModule, "BASS_ChannelFlags");
    pOrigBASS_StreamFree = (fpBASS_StreamFree)requireProc(bassModule, "BASS_StreamFree");
    pOrigBASS_Mixer_StreamAddChannel = (fpBASS_Mixer_StreamAddChannel)requireProc(bassMixModule, "BASS_Mixer_StreamAddChannel");
    pOrigBASS_Mixer_ChannelFlags = (fpBASS_Mixer_ChannelFlags)requireProc(bassMixModule, "BASS_Mixer_ChannelFlags");
    pOrigBASS_Mixer_ChannelRemove = (fpBASS_Mixer_ChannelRemove)requireProc(bassMixModule, "BASS_Mixer_ChannelRemove");

    if (pOrigBASS_SampleLoad == NULL || pOrigBASS_SampleGetInfo == NULL || pOrigBASS_SampleGetData == NULL || pOrigBASS_SampleFree == NULL ||
        pOrigBASS_SampleGetChannel == NULL || pOrigBASS_ChannelPlay == NULL || pOrigBASS_ChannelPause == NULL || pOrigBASS_ChannelStop == NULL ||
        pOrigBASS_ChannelIsActive == NULL ||
        pOrigBASS_ChannelSetAttribute == NULL || pOrigBASS_ChannelFlags == NULL ||
        pOrigBASS_StreamFree == NULL || pOrigBASS_Mixer_StreamAddChannel == NULL || pOrigBASS_Mixer_ChannelFlags == NULL ||
        pOrigBASS_Mixer_ChannelRemove == NULL)
        return;

    if (MH_CreateHook((LPVOID)pOrigBASS_SampleLoad, (LPVOID)MyBASS_SampleLoad, (PVOID *)&pOrigBASS_SampleLoad) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_SampleFree, (LPVOID)MyBASS_SampleFree, (PVOID *)&pOrigBASS_SampleFree) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_SampleGetChannel, (LPVOID)MyBASS_SampleGetChannel, (PVOID *)&pOrigBASS_SampleGetChannel) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_ChannelPlay, (LPVOID)MyBASS_ChannelPlay, (PVOID *)&pOrigBASS_ChannelPlay) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_ChannelPause, (LPVOID)MyBASS_ChannelPause, (PVOID *)&pOrigBASS_ChannelPause) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_ChannelStop, (LPVOID)MyBASS_ChannelStop, (PVOID *)&pOrigBASS_ChannelStop) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_ChannelSetAttribute, (LPVOID)MyBASS_ChannelSetAttribute, (PVOID *)&pOrigBASS_ChannelSetAttribute) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_ChannelFlags, (LPVOID)MyBASS_ChannelFlags, (PVOID *)&pOrigBASS_ChannelFlags) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_StreamFree, (LPVOID)MyBASS_StreamFree, (PVOID *)&pOrigBASS_StreamFree) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_Mixer_StreamAddChannel, (LPVOID)MyBASS_Mixer_StreamAddChannel, (PVOID *)&pOrigBASS_Mixer_StreamAddChannel) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_Mixer_ChannelFlags, (LPVOID)MyBASS_Mixer_ChannelFlags, (PVOID *)&pOrigBASS_Mixer_ChannelFlags) != MH_OK ||
        MH_CreateHook((LPVOID)pOrigBASS_Mixer_ChannelRemove, (LPVOID)MyBASS_Mixer_ChannelRemove, (PVOID *)&pOrigBASS_Mixer_ChannelRemove) != MH_OK)
    {
        logPrintf("[ASIO Hook] Hook creation failed\n");
        return;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        return;
    HMODULE pinned;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCSTR>(&initDLL), &pinned);

    pollerRunning = true;
    HANDLE pollThread = CreateThread(0, 0, monitorChannels, 0, 0, 0);
    if (pollThread != NULL)
    {
        hooksInitialised = true;
        CloseHandle(pollThread);
    }
    else
    {
        pollerRunning = false;
        hooksInitialised = false;
        InterlockedExchange(&MyPool->backendReady, 0);
        logPrintf("[ASIO Hook] monitor creation failed; keeping native audio\n");
    }
}

static DWORD WINAPI hookStartup(LPVOID)
{
    // Initialise diagnostics outside loader lock, before any detours can run.
    if(debugLogEnabled) {
        wchar_t module[MAX_PATH];
        DWORD length=GetModuleFileNameW(selfModule,module,MAX_PATH);
        wchar_t* slash=length && length<MAX_PATH ? wcsrchr(module,L'\\') : nullptr;
        if(slash) {
            *(slash+1)=0;
            std::wstring path=std::wstring(module)+L"asio_hook.log";
            hookTextLog=new(std::nothrow) asio::AsyncTextLog;
            if(hookTextLog) hookTextLog->startFile(path.c_str());
        }
    }
    logPrintf("[ASIO Hook] DLL attached pid=%lu; diagnostic sink is asynchronous\n", (unsigned long)GetCurrentProcessId());
    char name[80]; asioLiveMapName(name, GetCurrentProcessId());
    HANDLE liveMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* live = liveMap ? static_cast<AsioLiveAttach*>(MapViewOfFile(liveMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AsioLiveAttach))) : nullptr;
    bool requested = live && live->magic == ASIO_LIVE_MAGIC && live->pid == GetCurrentProcessId();
    if (requested) liveManagedReady = false;
    initDLL();
    if (requested) {
        int result = E_FAIL;
        if (hooksInitialised) {
            InterlockedExchange(&live->status, 1);
            wchar_t path[MAX_PATH];
            DWORD length = GetModuleFileNameW(selfModule, path, MAX_PATH);
            wchar_t* slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
            if (slash) {
                *slash = 0;
                std::wstring root = path;
                std::wstring assembly = root + L"\\managed\\Asio.ManagedBridge.dll";
                result = asio::loadExistingManaged(assembly.c_str(), L"LiveEntry, Asio.ManagedBridge", L"Initialize",
                    const_cast<wchar_t*>(root.c_str()), static_cast<int>((root.size() + 1) * sizeof(wchar_t)));
            }
        }
        liveManagedReady = result == 0;
        InterlockedExchange(&live->error, result);
        InterlockedExchange(&live->status, result == 0 ? 2 : -1);
        logPrintf("[ASIO Hook] live managed attach result=0x%08x\n", result);
    }
    if (live) UnmapViewOfFile(live);
    if (liveMap) CloseHandle(liveMap);
    return 0;
}

INT APIENTRY DllMain(HMODULE hDLL, DWORD Reason, LPVOID Reserved)
{
    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        selfModule = hDLL;
        InitializeCriticalSection(&logLock);
        logLockInitialised = true;
        initLogPath();
        initDebugLogSetting();
        logPrintf("[ASIO Hook] DLL attached pid=%lu\n", (unsigned long)GetCurrentProcessId());
        DisableThreadLibraryCalls(hDLL);
        HANDLE h1 = CreateThread(0, 0, hookStartup, 0, 0, 0);
        if (h1 != NULL)
            CloseHandle(h1);
        break;
    }

    case DLL_PROCESS_DETACH:
        // Successful hooks pin this DLL. At process termination other threads
        // are gone; avoid waiting on locks they may have held under loader lock.
        if (Reserved != NULL)
            return TRUE;
        logPrintf("[ASIO Hook] DLL detaching pid=%lu\n", (unsigned long)GetCurrentProcessId());
        pollerRunning = false;

        if (hooksInitialised)
        {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
        }

        if (locksInitialised)
        {
            EnterCriticalSection(&stateLock);
            for (auto it = sampleInfoMap.begin(); it != sampleInfoMap.end(); ++it)
            {
                if (it->second.mappingHandle != NULL)
                    CloseHandle(it->second.mappingHandle);
            }
            sampleInfoMap.clear();
            channelInfoMap.clear();
            LeaveCriticalSection(&stateLock);
        }

        if (lpBase != NULL)
            UnmapViewOfFile(lpBase);
        if (hMapFile != NULL)
            CloseHandle(hMapFile);

        if (locksInitialised)
        {
            DeleteCriticalSection(&queueLock);
            DeleteCriticalSection(&stateLock);
        }
        if (logLockInitialised)
        {
            DeleteCriticalSection(&logLock);
            logLockInitialised = false;
        }
        break;
    }

    return TRUE;
}
