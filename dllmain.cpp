#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <unordered_map>
#include <string>
#include <vector>

#include "MinHook.h"
#include "sharepool.h"

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
    float VAM;
    float Priority;
};

struct SampleInfo
{
    DWORD sampleRate;
    DWORD channels;
    DWORD format;
    DWORD dataLength;
    DWORD flags;
    string mappingName;
    HANDLE mappingHandle;

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
static bool hooksInitialised = false;
static bool locksInitialised = false;
static bool logLockInitialised = false;
static volatile bool pollerRunning = false;
static HMODULE selfModule = NULL;
static char logFilePath[MAX_PATH] = {0};
static char configFilePath[MAX_PATH] = {0};
static int debugLogEnabled = 0;

static unordered_map<HSAMPLE, SampleInfo> sampleInfoMap;
static unordered_map<HCHANNEL, ChannelInfo> channelInfoMap;

static void publishChannelState(HCHANNEL channel, DWORD state);

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

    EnterCriticalSection(&logLock);

    FILE *fp = fopen(logFilePath, "a");
    if (fp != NULL)
    {
        fputs(buffer, fp);
        fclose(fp);
    }

    LeaveCriticalSection(&logLock);
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
    if (MyPool == NULL)
        return false;

    EnterCriticalSection(&queueLock);

    LONG next = (MyPool->tail + 1) % EVENT_POOL_SIZE;
    if (next == MyPool->head)
    {
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
    sprintf(mappingName, "Local\\OsuAsioSample_%lu_%lu", GetCurrentProcessId(), (unsigned long)sample);

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

    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_SAMPLE_LOADED;
    event.data.sampleLoaded.sample = sample;
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
        vector<HCHANNEL> playingChannels;
        vector<HCHANNEL> pausedChannels;

        EnterCriticalSection(&stateLock);
        for (auto &entry : channelInfoMap)
        {
            DWORD active = pOrigBASS_ChannelIsActive != NULL ? pOrigBASS_ChannelIsActive(entry.first) : 0;
            bool shouldPlay = active == BASS_ACTIVE_PLAYING || active == BASS_ACTIVE_STALLED;

            if (shouldPlay && entry.second.paused)
            {
                entry.second.paused = false;
                playingChannels.push_back(entry.first);
            }
            else if (!shouldPlay && !entry.second.paused)
            {
                entry.second.paused = true;
                pausedChannels.push_back(entry.first);
            }
        }
        LeaveCriticalSection(&stateLock);

        for (HCHANNEL channel : playingChannels)
            publishChannelState(channel, CHANNEL_STATE_PLAYING);

        for (HCHANNEL channel : pausedChannels)
            publishChannelState(channel, CHANNEL_STATE_PAUSED);

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
    event.data.channelUpdated.channel = channel;
    event.data.channelUpdated.updateMask = mask;
    event.data.channelUpdated.volume = info.volume;
    event.data.channelUpdated.pan = info.pan;
    event.data.channelUpdated.frequency = info.frequency;
    event.data.channelUpdated.looping = info.looping ? 1 : 0;
    pushEvent(event);
}

static void publishChannelState(HCHANNEL channel, DWORD state)
{
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_CHANNEL_STATE;
    event.data.channelState.channel = channel;
    event.data.channelState.state = state;
    event.data.channelState.time = DETAILOUTPUT ? CPUclock() : 0;
    pushEvent(event);
}

static void publishChannelRemoved(HCHANNEL channel)
{
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = EVENT_CHANNEL_REMOVED;
    event.data.channelRemoved.channel = channel;
    pushEvent(event);
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
        MessageBoxA(NULL, message, "Info", MB_ICONEXCLAMATION);
    }

    return proc;
}

HSAMPLE WINAPI MyBASS_SampleLoad(BOOL mem, const void *file, QWORD offset, DWORD length, DWORD max, DWORD flags)
{
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
    logPrintf("[ASIO Hook] BASS_SampleFree sample=%lu\n", (unsigned long)handle);
    publishSampleFreed(handle);

    EnterCriticalSection(&stateLock);
    auto sampleIt = sampleInfoMap.find(handle);
    if (sampleIt != sampleInfoMap.end())
    {
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

    return pOrigBASS_SampleFree(handle);
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
    channelInfoMap[channel] = info;
    LeaveCriticalSection(&stateLock);

    publishChannelCreated(handle, channel, flags);
    publishChannelUpdated(channel, CHANNEL_UPDATE_LOOP, info);
    return channel;
}

BOOL WINAPI MyBASS_ChannelPlay(DWORD handle, BOOL restart)
{
    BOOL result = pOrigBASS_ChannelPlay(handle, restart);
    if (!result)
        return result;

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(handle);
    if (it != channelInfoMap.end())
    {
        it->second.paused = false;
        publishChannelState((HCHANNEL)handle, CHANNEL_STATE_PLAYING);
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

BOOL WINAPI MyBASS_ChannelPause(DWORD handle)
{
    BOOL result = pOrigBASS_ChannelPause(handle);
    if (!result)
        return result;

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(handle);
    if (it != channelInfoMap.end())
    {
        it->second.paused = true;
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

    publishChannelRemoved((HCHANNEL)handle);

    EnterCriticalSection(&stateLock);
    channelInfoMap.erase(handle);
    LeaveCriticalSection(&stateLock);

    return result;
}

BOOL WINAPI MyBASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value)
{
    BOOL result = pOrigBASS_ChannelSetAttribute(handle, attrib, value);
    if (!result)
        return result;

    EnterCriticalSection(&stateLock);
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
    BOOL result = pOrigBASS_Mixer_StreamAddChannel(handle, channel, flags);
    if (!result)
        return result;

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    if (it != channelInfoMap.end())
    {
        it->second.paused = (flags & BASS_MIXER_CHAN_PAUSE) != 0;
        publishChannelState((HCHANNEL)channel, it->second.paused ? CHANNEL_STATE_PAUSED : CHANNEL_STATE_PLAYING);
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

DWORD WINAPI MyBASS_Mixer_ChannelFlags(DWORD handle, DWORD flags, DWORD mask)
{
    DWORD result = pOrigBASS_Mixer_ChannelFlags(handle, flags, mask);

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(handle);
    if (it != channelInfoMap.end() && (mask & BASS_MIXER_CHAN_PAUSE) != 0)
    {
        bool paused = (result & BASS_MIXER_CHAN_PAUSE) != 0;
        if (paused != it->second.paused)
        {
            it->second.paused = paused;
            publishChannelState((HCHANNEL)handle, paused ? CHANNEL_STATE_PAUSED : CHANNEL_STATE_PLAYING);
        }
    }
    LeaveCriticalSection(&stateLock);

    return result;
}

BOOL WINAPI MyBASS_Mixer_ChannelRemove(DWORD handle)
{
    publishChannelRemoved((HCHANNEL)handle);

    EnterCriticalSection(&stateLock);
    channelInfoMap.erase(handle);
    LeaveCriticalSection(&stateLock);

    return pOrigBASS_Mixer_ChannelRemove(handle);
}

BOOL WINAPI MyBASS_StreamFree(DWORD handle)
{
    publishChannelRemoved((HCHANNEL)handle);

    EnterCriticalSection(&stateLock);
    channelInfoMap.erase(handle);
    LeaveCriticalSection(&stateLock);

    return pOrigBASS_StreamFree(handle);
}

extern "C" __declspec(dllexport) void initDLL()
{
    HMODULE bassModule = NULL;
    HMODULE bassMixModule = NULL;

    if (!waitForModule("bass.dll", &bassModule) || !waitForModule("bassmix.dll", &bassMixModule))
    {
        MessageBoxA(NULL, "bass.dll or bassmix.dll was not loaded in time", "Info", MB_ICONEXCLAMATION);
        return;
    }

    InitializeCriticalSection(&queueLock);
    InitializeCriticalSection(&stateLock);
    locksInitialised = true;

    hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct sharepool), "ShareMemoryForOsuASIO4Play");
    lpBase = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!(hMapFile && lpBase))
    {
        MessageBoxA(NULL, "ShareMemory Failed", "Info", MB_ICONEXCLAMATION);
        return;
    }

    MyPool = (struct sharepool *)lpBase;

    if (MH_Initialize() != MH_OK)
    {
        MessageBoxA(NULL, "MinHook initialize failed", "Info", MB_ICONEXCLAMATION);
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
        MessageBoxA(NULL, "Hook creation failed", "Info", MB_ICONEXCLAMATION);
        return;
    }

    MH_EnableHook(MH_ALL_HOOKS);
    hooksInitialised = true;

    pollerRunning = true;
    HANDLE pollThread = CreateThread(0, 0, monitorChannels, 0, 0, 0);
    if (pollThread != NULL)
        CloseHandle(pollThread);
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
        HANDLE h1 = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)initDLL, 0, 0, 0);
        if (h1 != NULL)
            CloseHandle(h1);
        break;
    }

    case DLL_PROCESS_DETACH:
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
