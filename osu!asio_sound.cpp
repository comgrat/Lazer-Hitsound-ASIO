#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>

#include <windows.h>
#include <winuser.h>
#include <TlHelp32.h>

#include "fmod/inc/fmod.h"
#include "fmod/inc/fmod_errors.h"
#include "sharepool.h"
#include "routing.h"
#include "audio_lease.h"
#include "live_attach.h"
#include "settings.h"
#include "async_text_log.h"
#include "i18n.h"
#include "event_wake.h"
#include "channel_failure.h"
#include "generation_history.h"
namespace ui=asio::i18n;

using namespace std;

struct SampleData
{
    FMOD_SOUND *sound;
    DWORD sampleRate;
    DWORD channels;
    DWORD format;
    bool looping;
    DWORD generation = 0;

    SampleData()
        : sound(nullptr), sampleRate(0), channels(0), format(SAMPLE_FORMAT_UNKNOWN), looping(false)
    {
    }
};

struct ChannelData
{
    DWORD generation = 0;
    HSAMPLE sample;
    FMOD_CHANNEL *channel;
    float volume;
    float pan;
    float frequency;
    bool looping;
    bool playing;
    enum class Phase { Fresh, Playing, Paused, Finished } phase = Phase::Fresh;
    HCHANNEL handle = 0;
    unsigned long long voiceId = 0;

    ChannelData()
        : sample(0), channel(nullptr), volume(1.0f), pan(0.0f), frequency(0.0f), looping(false), playing(false)
    {
    }
};

static FMOD_SYSTEM *fmodSystem = nullptr;
static FMOD_CHANNELGROUP *asioOutputGroup = nullptr;
static struct sharepool *MyPool = nullptr;
static std::atomic<bool> Work{false};
static std::atomic<bool> backendFault{false};
static DWORD mainThreadId = 0;
static HANDLE backendWake = nullptr;
static bool environmentDiagnostics = false;
static asio::QueueLatency queueLatency;

static int bufferSize = 128;
static int driverId = 0;
static int sampleRate = 48000;
static int outputVolume = 100;
static int debugLogEnabled = 0;
// Opt-in per-event trace (playback-trace.enabled beside the tool). Debug Log
// alone keeps the periodic status, counters and real failures readable.
static bool detailedTraceEnabled = false;
static std::wstring configPath;
static asio::Settings startupSettings;
static std::atomic<int> requestedOutputVolume{100};
static std::atomic<int> requestedDebugLog{0};
static std::atomic<bool> settingsWatcherRunning{false};
static HANDLE settingsStopEvent = nullptr;
static asio::AsyncTextLog* playbackLog=nullptr;

static DWORD WINAPI watchSettings(LPVOID)
{
    std::string lastError,previousSnapshot;
    bool haveSnapshot=false;
    while (settingsWatcherRunning) {
        asio::Settings next;
        std::string error,snapshot;
        if (asio::loadSettings(configPath, next, error,&snapshot)) {
            lastError.clear();
            // Two identical closed-writer snapshots avoid applying transient
            // save contents. A valid file left stable is treated as a save.
            if(haveSnapshot && previousSnapshot==snapshot) {
                requestedOutputVolume.store(next.volume, std::memory_order_release);
                requestedDebugLog.store(next.debug, std::memory_order_release);
            }
            previousSnapshot=std::move(snapshot);haveSnapshot=true;
        } else {
            haveSnapshot=false;
            if(error!=lastError){
                fprintf(stderr,"%s\n",ui::text("backend.config_rejected",{error}).c_str());
                lastError=error;
            }
        }
        if (WaitForSingleObject(settingsStopEvent, 250) != WAIT_TIMEOUT) break;
    }
    return 0;
}

static bool refreshOutputVolume()
{
    int nextVolume=requestedOutputVolume.load(std::memory_order_acquire);
    debugLogEnabled=requestedDebugLog.load(std::memory_order_acquire);
    if (MyPool) InterlockedExchange(&MyPool->diagnosticsEnabled, environmentDiagnostics || debugLogEnabled);
    if(nextVolume==outputVolume) return true;
    if(!asioOutputGroup) return false;
    FMOD_RESULT result=FMOD_ChannelGroup_SetVolume(asioOutputGroup,nextVolume/100.0f);
    if(result!=FMOD_OK) return false;
    outputVolume=nextVolume;
    if(playbackLog) playbackLog->write("%s\n",ui::text("backend.volume_applied",{std::to_string(outputVolume),asio::utf8Path(configPath)}).c_str());
    return true;
}
// Use the same stereo output as the independent left/right device check.
// A driver's reported channel count describes its capabilities, not our mix.
struct PlaybackStats
{
    unsigned long long samplesLoaded = 0;
    unsigned long long playRequests = 0;
    unsigned long long starts = 0;
    unsigned long long missingSamples = 0;
    unsigned long long staleRejected = 0;
    unsigned long long finished = 0;
};
static PlaybackStats playbackStats; // Accessed only by the event worker.
static unsigned long long nextVoiceId = 0;

static void traceVoice(const char *action, const ChannelData &data)
{
    if (!debugLogEnabled || !detailedTraceEnabled) return;
    if(!playbackLog)return;
    playbackLog->write("{\"event\":\"voice_%s\",\"tick_ms\":%llu,\"channel\":%lu,\"generation\":%lu,\"sample\":%lu,\"voice\":%llu,\"sample_gain\":%.6f,\"asio_percent\":%d}\n",
        action, GetTickCount64(), static_cast<unsigned long>(data.handle), static_cast<unsigned long>(data.generation),
        static_cast<unsigned long>(data.sample), data.voiceId, data.volume, outputVolume);
}

static void reportPlaybackStats()
{
    if(playbackLog) playbackLog->write("[ASIO status] ready=%ld samples=%llu PLAY=%llu started=%llu finished=%llu missingSample=%llu staleRejected=%llu\n",
           MyPool->backendReady, playbackStats.samplesLoaded, playbackStats.playRequests,
           playbackStats.starts, playbackStats.finished, playbackStats.missingSamples, playbackStats.staleRejected);
    if (MyPool && MyPool->diagnosticsEnabled && playbackLog)
    {
        auto delay = queueLatency.summary();
        playbackLog->write("[ASIO status] queue_start_ms n=%zu p50=%.4f p95=%.4f max=%.4f (last <=4096 diagnostic PLAY events)\n",
            delay.count, delay.p50, delay.p95, delay.maximum);
    }
}

static void debugPrintf(const char *format, ...)
{
    if (!debugLogEnabled)
        return;

    va_list args;
    va_start(args, format);
    if(playbackLog) playbackLog->writeV(format,args);
    va_end(args);
}

// Per-event lines: off unless both Debug Log and the trace marker are set.
static void tracePrintf(const char *format, ...)
{
    if (!debugLogEnabled || !detailedTraceEnabled || !playbackLog)
        return;

    va_list args;
    va_start(args, format);
    playbackLog->writeV(format,args);
    va_end(args);
}

static bool init()
{
    configPath=asio::executableSibling(L"config.ini");
    std::string error;
    if(configPath.empty()||!asio::loadSettings(configPath,startupSettings,error)) {
        fprintf(stderr,"%s\n",ui::text("backend.config_failed",{error,asio::utf8Path(configPath)}).c_str());
        return false;
    }
    bufferSize=startupSettings.buffer; driverId=startupSettings.driver; sampleRate=startupSettings.rate;
    outputVolume=startupSettings.volume; debugLogEnabled=startupSettings.debug;
    requestedOutputVolume=outputVolume; requestedDebugLog=debugLogEnabled;
    const auto traceMarker=asio::executableSibling(L"playback-trace.enabled");
    detailedTraceEnabled=!traceMarker.empty()&&GetFileAttributesW(traceMarker.c_str())!=INVALID_FILE_ATTRIBUTES;
    puts(ui::text("backend.config_loaded",{std::to_string(GetCurrentProcessId()),asio::utf8Path(configPath),std::to_string(outputVolume)}).c_str());
    return true;
}

static FMOD_SOUND_FORMAT toFmodFormat(DWORD format)
{
    switch (format)
    {
    case SAMPLE_FORMAT_PCM8:
        return FMOD_SOUND_FORMAT_PCM8;
    case SAMPLE_FORMAT_PCM16:
        return FMOD_SOUND_FORMAT_PCM16;
    case SAMPLE_FORMAT_PCM24:
        return FMOD_SOUND_FORMAT_PCM24;
    case SAMPLE_FORMAT_PCM32:
        return FMOD_SOUND_FORMAT_PCM32;
    case SAMPLE_FORMAT_FLOAT:
        return FMOD_SOUND_FORMAT_PCMFLOAT;
    default:
        return FMOD_SOUND_FORMAT_NONE;
    }
}

static void backendFailed(FMOD_RESULT result)
{
    bool wasReady = !MyPool || InterlockedExchange(&MyPool->backendReady, 0) != 0;
    if (wasReady)
        fprintf(stderr,"%s\n",ui::text("backend.disabled",{FMOD_ErrorString(result)}).c_str());
    FMOD_CHANNELGROUP *master = nullptr;
    if (fmodSystem && FMOD_System_GetMasterChannelGroup(fmodSystem, &master) == FMOD_OK)
        FMOD_ChannelGroup_SetMute(master, true);
}

static void finishChannel(ChannelData &data, FMOD_RESULT reason)
{
    if (data.phase == ChannelData::Phase::Finished) return;
    data.channel = nullptr;
    data.playing = false;
    data.phase = ChannelData::Phase::Finished;
    ++playbackStats.finished;
    traceVoice("finished", data);
    if (data.looping)
    {
        debugPrintf("[FMOD] loop voice retired channel=%lu generation=%lu reason=%d; requesting native failover\n",
            data.handle, data.generation, int(reason));
        if (!publishChannelFailure(MyPool, data.handle, data.generation)) backendFailed(FMOD_ERR_INTERNAL);
    }
}

static bool refreshChannelLifetime(ChannelData &data)
{
    if (!data.channel || data.phase == ChannelData::Phase::Finished) return false;
    // Newly allocated voices are paused before their first PLAY. A later
    // inactive voice can still have a valid FMOD handle and accept setters.
    if (data.phase == ChannelData::Phase::Fresh) return true;
    FMOD_BOOL active = false;
    FMOD_RESULT result = FMOD_Channel_IsPlaying(data.channel, &active);
    if (result == FMOD_ERR_INVALID_HANDLE || result == FMOD_ERR_CHANNEL_STOLEN || (result == FMOD_OK && !active))
    {
        finishChannel(data, result);
        return false;
    }
    if (result != FMOD_OK) { backendFailed(result); return false; }
    return true;
}

static bool applyChannelState(ChannelData &channelData)
{
    if (!refreshChannelLifetime(channelData))
        return false;

    // Sample gain and independent ASIO gain live at distinct levels. The
    // group can update every playing voice without multiplying the gain twice.
    FMOD_RESULT result = FMOD_Channel_SetVolume(channelData.channel, asio::outputGain(channelData.volume, 100));
    if (result == FMOD_OK) result = FMOD_Channel_SetPan(channelData.channel, channelData.pan);

    if (result == FMOD_OK && channelData.frequency > 0)
        result = FMOD_Channel_SetFrequency(channelData.channel, channelData.frequency);

    if (result == FMOD_OK) result = FMOD_Channel_SetMode(channelData.channel, channelData.looping ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF);
    if (result == FMOD_OK) result = FMOD_Channel_SetPriority(channelData.channel, channelData.looping ? 0 : 128);
    if (result == FMOD_ERR_INVALID_HANDLE || result == FMOD_ERR_CHANNEL_STOLEN)
    {
        finishChannel(channelData, result);
        return false;
    }
    if (result != FMOD_OK) backendFailed(result);
    return result == FMOD_OK;
}

static sharepool::SampleAck *findSampleAck(HSAMPLE sample)
{
    for (int probe = 0; probe < SAMPLE_ACK_COUNT; ++probe)
    {
        auto &ack = MyPool->sampleAcks[(sample + probe) % SAMPLE_ACK_COUNT];
        if (DWORD(ack.sample) == sample) return &ack;
    }
    return nullptr;
}

static bool loadSampleData(const SampleLoadedPayload &payload, unordered_map<HSAMPLE, SampleData> &samples)
{
    auto *currentAck = findSampleAck(payload.sample);
    if (!currentAck || DWORD(currentAck->requestedGeneration) != payload.generation)
        return false;
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, payload.mappingName);
    if (mapping == NULL)
        return false;

    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, payload.dataLength);
    if (view == NULL)
    {
        CloseHandle(mapping);
        return false;
    }

    SampleData sampleData;
    sampleData.sampleRate = payload.sampleRate;
    sampleData.channels = payload.channels;
    sampleData.format = payload.format;
    sampleData.generation = payload.generation;

    FMOD_CREATESOUNDEXINFO exinfo;
    memset(&exinfo, 0, sizeof(exinfo));
    exinfo.cbsize = sizeof(exinfo);
    exinfo.length = payload.dataLength;
    exinfo.numchannels = payload.channels;
    exinfo.defaultfrequency = payload.sampleRate;
    exinfo.format = toFmodFormat(payload.format);

    if (exinfo.format == FMOD_SOUND_FORMAT_NONE)
    {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }

    FMOD_RESULT result = FMOD_System_CreateSound(
        fmodSystem,
        static_cast<const char *>(view),
        FMOD_OPENMEMORY | FMOD_OPENRAW | FMOD_CREATESAMPLE | FMOD_LOWMEM | FMOD_LOOP_OFF,
        &exinfo,
        &sampleData.sound);

    // FMOD_OPENMEMORY + CREATESAMPLE copies the source before returning.
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    if (result != FMOD_OK)
    {
        debugPrintf("[FMOD] CreateSound failed: %s\n", FMOD_ErrorString(result));
        return false;
    }

    auto existing = samples.find(payload.sample);
    if (existing != samples.end() && existing->second.sound != nullptr)
        FMOD_Sound_Release(existing->second.sound);

    samples[payload.sample] = std::move(sampleData);
    auto *ack = findSampleAck(payload.sample);
    if (ack && DWORD(ack->requestedGeneration) == payload.generation)
        InterlockedExchange(&ack->readyGeneration, LONG(payload.generation));
    return true;
}

static void removeSample(HSAMPLE sample, unordered_map<HSAMPLE, SampleData> &samples, unordered_map<HCHANNEL, ChannelData> &channels)
{
    for (auto it = channels.begin(); it != channels.end();)
    {
        if (it->second.sample == sample)
        {
            if (it->second.channel != nullptr)
                FMOD_Channel_Stop(it->second.channel);
            it = channels.erase(it);
        }
        else
            ++it;
    }

    auto sampleIt = samples.find(sample);
    if (sampleIt != samples.end())
    {
        if (sampleIt->second.sound != nullptr)
            FMOD_Sound_Release(sampleIt->second.sound);
        samples.erase(sampleIt);
    }
}

static void ensurePlaying(ChannelData &channelData, SampleData &sampleData)
{
    if (sampleData.sound == nullptr || channelData.phase == ChannelData::Phase::Finished)
        return;
    if (channelData.channel && !refreshChannelLifetime(channelData)) return;
    if (channelData.phase == ChannelData::Phase::Playing) {
        traceVoice("duplicate_play_ignored",channelData);
        return;
    }

    if (channelData.channel == nullptr)
    {
        FMOD_CHANNEL *newChannel = nullptr;
        FMOD_RESULT result = FMOD_System_PlaySound(fmodSystem, sampleData.sound, nullptr, true, &newChannel);
        if (result != FMOD_OK)
        {
            debugPrintf("[FMOD] PlaySound failed: %s\n", FMOD_ErrorString(result));
            backendFailed(result);
            return;
        }

        channelData.channel = newChannel;
        channelData.voiceId = ++nextVoiceId;
        traceVoice("create",channelData);
        if (!applyChannelState(channelData)) return;
        tracePrintf("[FMOD] Channel created for sample %lu\n", (unsigned long)channelData.sample);
    }

    FMOD_RESULT result = FMOD_Channel_SetPaused(channelData.channel, false);
    if (result != FMOD_OK)
    {
        debugPrintf("[FMOD] SetPaused(false) failed: %s\n", FMOD_ErrorString(result));
        if (result == FMOD_ERR_INVALID_HANDLE || result == FMOD_ERR_CHANNEL_STOLEN)
        {
            finishChannel(channelData, result);
            return;
        }
        backendFailed(result);
        return;
    }

    channelData.playing = true;
    channelData.phase = ChannelData::Phase::Playing;
    traceVoice("unpause",channelData);
    ++playbackStats.starts;
    // These getters only feed a trace line. Without the trace marker they
    // would cost three FMOD calls per hit on the event worker.
    if (debugLogEnabled && detailedTraceEnabled)
    {
        FMOD_BOOL paused = false;
        FMOD_BOOL isPlaying = false;
        float audibility = 0.0f;
        FMOD_Channel_GetPaused(channelData.channel, &paused);
        FMOD_Channel_IsPlaying(channelData.channel, &isPlaying);
        FMOD_Channel_GetAudibility(channelData.channel, &audibility);
        tracePrintf("[FMOD] Channel %lu playing paused=%d isPlaying=%d audibility=%.4f\n",
                    (unsigned long)channelData.sample,
                    paused,
                    isPlaying,
                    audibility);
    }
}

static void ensurePaused(ChannelData &channelData)
{
    if (channelData.phase == ChannelData::Phase::Finished || channelData.phase == ChannelData::Phase::Paused)
        return;
    if (!channelData.channel && channelData.phase == ChannelData::Phase::Fresh) {
        channelData.playing = false; // Paused-before-first-PLAY is still fresh.
        return;
    }
    if (channelData.channel != nullptr)
    {
        if (!refreshChannelLifetime(channelData)) return;
        FMOD_RESULT result = FMOD_Channel_SetPaused(channelData.channel, true);
        if (result != FMOD_OK)
        {
            if (result == FMOD_ERR_INVALID_HANDLE || result == FMOD_ERR_CHANNEL_STOLEN)
            {
                finishChannel(channelData, result);
                return;
            }
            else { backendFailed(result); return; }
        }
    }
    channelData.playing = false;
    channelData.phase = channelData.channel ? ChannelData::Phase::Paused : ChannelData::Phase::Finished;
    traceVoice(channelData.channel ? "pause" : "finished",channelData);
}

static void removeChannel(HCHANNEL channel, unordered_map<HCHANNEL, ChannelData> &channels)
{
    auto channelIt = channels.find(channel);
    if (channelIt == channels.end())
        return;

    if (channelIt->second.channel != nullptr)
        FMOD_Channel_Stop(channelIt->second.channel);

    channels.erase(channelIt);
}

static void retireFinishedChannels(unordered_map<HCHANNEL, ChannelData> &channels,
                                   GenerationHistory &generations)
{
    for (auto it = channels.begin(); it != channels.end();)
    {
        auto &data = it->second;
        if (data.channel && data.phase == ChannelData::Phase::Playing) refreshChannelLifetime(data);
        if (data.phase != ChannelData::Phase::Finished) { ++it; continue; }
        // Keep only the small replay guard. Removing it without an epoch or
        // watermark protocol would let late same-generation PLAY allocate again.
        generations.retire(it->first, data.generation);
        it = channels.erase(it);
    }
}

static DWORD WINAPI mainloop(LPVOID)
{
    unordered_map<HSAMPLE, SampleData> samples;
    unordered_map<HCHANNEL, ChannelData> channels;
    GenerationHistory generations;
    DWORD lastReport = GetTickCount();

    while (Work)
    {
        InterlockedExchange(&MyPool->heartbeat, LONG(GetTickCount()));
        if (!refreshOutputVolume()) { backendFailed(FMOD_ERR_INTERNAL); }
        if (DWORD(GetTickCount() - lastReport) >= 5000)
        {
            reportPlaybackStats();
            lastReport = GetTickCount();
        }
        if (!MyPool->backendReady)
        {
            for (auto &entry : channels) ensurePaused(entry.second);
            // A lost event invalidates channel state. Fail closed rather than
            // reset readiness and replay stale state, or report success forever.
            backendFault = true;
            fprintf(stderr,"%s\n",ui::text("backend.fault").c_str());
            Work = false;
            PostThreadMessage(mainThreadId, WM_QUIT, 0, 0);
            break;
        }
        bool processedEvent = false;

        while (MyPool->head != MyPool->tail)
        {
            if (!MyPool->backendReady)
                break;
            InterlockedExchange(&MyPool->heartbeat, LONG(GetTickCount()));
            Event event = MyPool->events[MyPool->head];
            MemoryBarrier();
            MyPool->head = (MyPool->head + 1) % EVENT_POOL_SIZE;
            processedEvent = true;
            if (event.type == EVENT_CHANNEL_STATE && event.data.channelState.state == CHANNEL_STATE_PLAYING &&
                MyPool->diagnosticsEnabled && event.data.channelState.time > 0)
                queueLatency.add(asio::qpcMilliseconds() - event.data.channelState.time);

            if (event.type >= EVENT_CHANNEL_CREATED && event.type <= EVENT_CHANNEL_REMOVED) {
                HCHANNEL handle = event.type == EVENT_CHANNEL_CREATED ? event.data.channelCreated.channel : event.data.channelUpdated.channel;
                auto tracked = channels.find(handle);
                bool sameLifetime = tracked != channels.end() && tracked->second.generation == event.generation;
                bool mayCreate = event.type == EVENT_CHANNEL_CREATED || event.type == EVENT_CHANNEL_REMOVED ||
                    (event.type == EVENT_CHANNEL_STATE && event.data.channelState.state == CHANNEL_STATE_PLAYING);
                if (!generations.accept(handle, event.generation, event.type == EVENT_CHANNEL_CREATED, sameLifetime, mayCreate))
                {
                    // Expected for every late event after natural completion;
                    // counted in [ASIO status], listed only in detailed traces.
                    ++playbackStats.staleRejected;
                    tracePrintf("[FMOD] stale/retired event ignored channel=%lu generation=%lu type=%lu\n", handle, event.generation, event.type);
                    continue;
                }
            }

            if (event.type >= EVENT_CHANNEL_UPDATED && event.type <= EVENT_CHANNEL_REMOVED)
            {
                HCHANNEL handle = event.data.channelUpdated.channel;
                auto current = channels.find(handle);
                if (current == channels.end() || current->second.generation != event.generation)
                {
                    // A PLAY carries its complete snapshot, so a dropped/delayed
                    // create or update cannot produce a muted or wrong-gain hit.
                    if (event.type != EVENT_CHANNEL_STATE || event.data.channelState.state != CHANNEL_STATE_PLAYING) {
                        if(event.type==EVENT_CHANNEL_REMOVED) generations.retire(handle, event.generation);
                        continue;
                    }
                    removeChannel(handle, channels);
                    ChannelData data;
                    data.sample = event.data.channelState.sample;
                    data.generation = event.generation;
                    data.handle = handle;
                    channels[handle] = data;
                }
            }

            switch (event.type)
            {
            case EVENT_SAMPLE_LOADED:
                if (loadSampleData(event.data.sampleLoaded, samples))
                {
                    ++playbackStats.samplesLoaded;
                    debugPrintf("[FMOD] Loaded sample %lu (%u bytes)\n", (unsigned long)event.data.sampleLoaded.sample, event.data.sampleLoaded.dataLength);
                    for (auto &entry : channels)
                    {
                        if (entry.second.sample != event.data.sampleLoaded.sample)
                            continue;

                        if (entry.second.playing)
                            ensurePlaying(entry.second, samples[event.data.sampleLoaded.sample]);
                        else if (entry.second.channel != nullptr)
                            ensurePaused(entry.second);
                    }
                }
                break;

            case EVENT_SAMPLE_FREED:
                for(const auto &entry:channels)
                    if(entry.second.sample==event.data.sampleFreed.sample) generations.retire(entry.first, entry.second.generation);
                removeSample(event.data.sampleFreed.sample, samples, channels);
                break;

            case EVENT_CHANNEL_CREATED:
            {
                removeChannel(event.data.channelCreated.channel, channels);
                ChannelData channelData;
                channelData.generation = event.generation;
                channelData.handle = event.data.channelCreated.channel;
                channelData.sample = event.data.channelCreated.sample;
                channelData.looping = (event.data.channelCreated.flags & BASS_SAMPLE_LOOP) != 0;
                channels[event.data.channelCreated.channel] = channelData;
                tracePrintf("[FMOD] Channel event created: sample=%lu channel=%lu flags=%u\n",
                            (unsigned long)event.data.channelCreated.sample,
                            (unsigned long)event.data.channelCreated.channel,
                            event.data.channelCreated.flags);
                break;
            }

            case EVENT_CHANNEL_UPDATED:
            {
                auto channelIt = channels.find(event.data.channelUpdated.channel);
                if (channelIt == channels.end())
                    break;

                ChannelData &channelData = channelIt->second;

                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_VOLUME)
                    channelData.volume = event.data.channelUpdated.volume;
                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_PAN)
                    channelData.pan = event.data.channelUpdated.pan;
                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_FREQUENCY)
                    channelData.frequency = event.data.channelUpdated.frequency;
                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_LOOP)
                    channelData.looping = event.data.channelUpdated.looping != 0;

                applyChannelState(channelData);
                break;
            }

            case EVENT_CHANNEL_STATE:
            {
                if (event.data.channelState.state == CHANNEL_STATE_PLAYING)
                    ++playbackStats.playRequests;
                auto channelIt = channels.find(event.data.channelState.channel);
                if (channelIt == channels.end())
                {
                    debugPrintf("[FMOD] Channel state ignored, missing channel %lu\n", (unsigned long)event.data.channelState.channel);
                    break;
                }

                channelIt->second.playing = event.data.channelState.state == CHANNEL_STATE_PLAYING;
                channelIt->second.volume = event.data.channelState.volume;
                channelIt->second.pan = event.data.channelState.pan;
                channelIt->second.frequency = event.data.channelState.frequency;
                channelIt->second.looping = event.data.channelState.looping != 0;

                auto sampleIt = samples.find(channelIt->second.sample);
                if (sampleIt == samples.end())
                {
                    if (channelIt->second.playing) ++playbackStats.missingSamples;
                    debugPrintf("[FMOD] Channel state deferred, missing sample %lu\n", (unsigned long)channelIt->second.sample);
                    break;
                }

                tracePrintf("[FMOD] Channel state: channel=%lu state=%u\n",
                            (unsigned long)event.data.channelState.channel,
                            event.data.channelState.state);

                if (channelIt->second.playing)
                    ensurePlaying(channelIt->second, sampleIt->second);
                else
                    ensurePaused(channelIt->second);

                break;
            }

            case EVENT_CHANNEL_REMOVED:
                generations.retire(event.data.channelRemoved.channel, event.generation);
                removeChannel(event.data.channelRemoved.channel, channels);
                break;

            default:
                break;
            }

        }

        FMOD_RESULT update = FMOD_System_Update(fmodSystem);
        if (update != FMOD_OK) backendFailed(update);

        // A naturally finished FMOD channel must never be revived by a late
        // attribute update or another queued state for the same generation.
        retireFinishedChannels(channels, generations);
        if (!processedEvent)
        {
            if (backendWake) WaitForSingleObject(backendWake, 5);
            else Sleep(1);
        }
    }
    InterlockedExchange(&MyPool->backendReady, 0);
    reportPlaybackStats();

    for (auto &entry : channels)
    {
        if (entry.second.channel != nullptr)
            FMOD_Channel_Stop(entry.second.channel);
    }

    for (auto &entry : samples)
    {
        if (entry.second.sound != nullptr)
            FMOD_Sound_Release(entry.second.sound);
    }
    return 0;
}

static BOOL WINAPI consoleControl(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
        return FALSE;
    Work = false;
    PostThreadMessage(mainThreadId, WM_QUIT, 0, 0);
    return TRUE;
}

static bool DllInject(HANDLE hProcess, const wchar_t *dllname)
{
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32 == 0)
        return 0;

    LPTHREAD_START_ROUTINE loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");
    if (loadLibrary == 0)
        return 0;

    SIZE_T dllNameLen = (wcslen(dllname) + 1) * sizeof(wchar_t);
    LPVOID remoteAddress = VirtualAllocEx(hProcess, NULL, dllNameLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteAddress == 0)
        return 0;

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, remoteAddress, (void *)dllname, dllNameLen, &bytesWritten))
    {
        VirtualFreeEx(hProcess, remoteAddress, 0, MEM_RELEASE);
        return 0;
    }

    HANDLE thread = CreateRemoteThread(hProcess, NULL, 0, loadLibrary, remoteAddress, 0, NULL);
    if (thread == 0)
    {
        VirtualFreeEx(hProcess, remoteAddress, 0, MEM_RELEASE);
        return 0;
    }

    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) {
        // The target may still be reading this buffer. Do not free it early.
        CloseHandle(thread);
        return false;
    }

    CloseHandle(thread);
    VirtualFreeEx(hProcess, remoteAddress, 0, MEM_RELEASE);
    // Remote thread exit codes are only 32-bit; enumerate the actual x64
    // module instead of treating a truncated HMODULE as a successful result.
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(hProcess));
    if (modules == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W module = {}; module.dwSize = sizeof(module);
    bool loaded = false;
    if (Module32FirstW(modules, &module)) do {
        if (!_wcsicmp(module.szExePath, dllname)) { loaded = true; break; }
    } while (Module32NextW(modules, &module));
    CloseHandle(modules);
    return loaded;
}

static DWORD getPID(LPCSTR ProcessName)
{
    HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(hProcessSnap, &pe32))
    {
        CloseHandle(hProcessSnap);
        return 0;
    }

    DWORD dwPid = 0;
    do
    {
        if (!strcmp(ProcessName, pe32.szExeFile))
        {
            dwPid = pe32.th32ProcessID;
            break;
        }
    } while (Process32Next(hProcessSnap, &pe32));

    CloseHandle(hProcessSnap);
    return dwPid;
}

static char osuExename[256] = "osu!.exe";

int main(int argc, char *argv[])
{
    // The launcher redirects these streams. Keep status and failures visible
    // while the game is running, rather than only when this process exits.
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    ui::initialize();
    bool liveAttach = false;
    DWORD requestedPid = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--live")) liveAttach = true;
        else if (!strcmp(argv[i], "--attach-pid") && i + 1 < argc) {
            char* end = nullptr;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (!value || !end || *end) return 2;
            requestedPid = DWORD(value);
        } else return 2;
    }
    char enabled[2] = {};
    const char* optIn = liveAttach ? "ASIO_STANDALONE_ENABLE" : "ASIO_OFFLINE_ENABLE";
    if ((liveAttach && !requestedPid) || GetEnvironmentVariableA(optIn, enabled, sizeof(enabled)) != 1 || enabled[0] != '1')
    {
        fprintf(stderr,"%s\n",ui::text("backend.internal").c_str());
        return 2;
    }
    asio::TimerResolution timerResolution;
    mainThreadId = GetCurrentThreadId();
    MSG initialMessage;
    PeekMessage(&initialMessage, NULL, 0, 0, PM_NOREMOVE);
    SetConsoleCtrlHandler(consoleControl, TRUE);
    // Preserve the original config format, but resolve it beside the executable.
    const auto directory=asio::executableDirectory();
    if (directory.empty() || !SetCurrentDirectoryW(directory.c_str())) return 1;
    if (!init()) return 1;
    printf("FMOD Studio Low Level API (C) Firelight Technologies Pty Ltd.\n");


    FMOD_RESULT initRet = FMOD_System_Create(&fmodSystem, FMOD_VERSION);
    if (initRet != FMOD_OK)
    {
        puts(ui::text("backend.create_failed",{FMOD_ErrorString(initRet)}).c_str());
        return 1;
    }

    initRet = FMOD_System_SetDSPBufferSize(fmodSystem, bufferSize, 2);
    if (initRet == FMOD_OK) initRet = FMOD_System_SetOutput(fmodSystem, FMOD_OUTPUTTYPE_ASIO);
    if (initRet == FMOD_OK) initRet = FMOD_System_SetDriver(fmodSystem, driverId);
    if (initRet != FMOD_OK)
    {
        fprintf(stderr,"%s\n",ui::text("backend.asio_failed",{FMOD_ErrorString(initRet)}).c_str());
        FMOD_System_Release(fmodSystem);
        return 1;
    }

    if (sampleRate==0) {
        initRet=FMOD_System_GetDriverInfo(fmodSystem,driverId,nullptr,0,nullptr,&sampleRate,nullptr,nullptr);
        if(initRet!=FMOD_OK || sampleRate<8000){fprintf(stderr,"%s\n",ui::text("backend.default_rate_failed").c_str());FMOD_System_Release(fmodSystem);return 1;}
    }
    initRet = FMOD_System_SetSoftwareFormat(fmodSystem, sampleRate, FMOD_SPEAKERMODE_STEREO, 0);
    if (initRet != FMOD_OK)
    {
        fprintf(stderr,"%s\n",ui::text("backend.stereo_failed",{FMOD_ErrorString(initRet)}).c_str());
        FMOD_System_Release(fmodSystem); return 1;
    }
    initRet = FMOD_System_Init(fmodSystem, 256, FMOD_INIT_NORMAL, 0);
    if (initRet != FMOD_OK)
    {
        puts(ui::text("backend.init_failed",{FMOD_ErrorString(initRet)}).c_str());
        FMOD_System_Release(fmodSystem);
        return 1;
    }

    initRet=FMOD_System_GetMasterChannelGroup(fmodSystem,&asioOutputGroup);
    if(initRet==FMOD_OK) initRet=FMOD_ChannelGroup_SetVolume(asioOutputGroup,outputVolume/100.0f);
    if(initRet!=FMOD_OK){fprintf(stderr,"%s\n",ui::text("backend.bus_failed",{FMOD_ErrorString(initRet)}).c_str());FMOD_System_Release(fmodSystem);return 1;}
    if (!liveAttach) Sleep(1000);

    unsigned bufLen;
    int bufNum;
    char name[256];
    int speakerChannels;
    FMOD_SPEAKERMODE speakerMode;
    FMOD_System_GetDSPBufferSize(fmodSystem, &bufLen, &bufNum);
    FMOD_System_GetDriverInfo(fmodSystem, driverId, name, 255, 0, 0, &speakerMode, &speakerChannels);
    int actualRate = 0, rawChannels = 0;
    FMOD_SPEAKERMODE actualMode = FMOD_SPEAKERMODE_DEFAULT;
    initRet = FMOD_System_GetSoftwareFormat(fmodSystem, &actualRate, &actualMode, &rawChannels);
    if (initRet != FMOD_OK)
    {
        fprintf(stderr,"%s\n",ui::text("backend.format_failed",{FMOD_ErrorString(initRet)}).c_str());
        FMOD_System_Release(fmodSystem); return 1;
    }
    puts(ui::text("backend.info",{name,std::to_string(driverId),std::to_string(outputVolume),std::to_string(actualRate),std::to_string(bufLen),std::to_string(bufNum)}).c_str());
    char bufferDuration[40];snprintf(bufferDuration,sizeof(bufferDuration),"%.4f",actualRate>0?bufLen*bufNum*1000.0/actualRate:0.0);
    puts(ui::text("backend.buffer_note",{bufferDuration}).c_str());

    // Normal same-user attachment does not need SeDebugPrivilege.
    DWORD PID = requestedPid;
    while (!PID)
    {
        PID = getPID(osuExename);
        Sleep(50);
    }
    if (!liveAttach) Sleep(1000);

    char poolName[128];
    snprintf(poolName, sizeof(poolName), "Local\\OsuAsioV2_%lu", PID);
    HANDLE hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct sharepool), poolName);
    if (!hMapFile || GetLastError() == ERROR_ALREADY_EXISTS)
    { if (hMapFile) CloseHandle(hMapFile); FMOD_System_Release(fmodSystem); return 1; }
    LPVOID lpBase = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct sharepool));
    if (!lpBase) { CloseHandle(hMapFile); FMOD_System_Release(fmodSystem); return 1; }
    MyPool = static_cast<sharepool *>(lpBase);
    memset(MyPool, 0, sizeof(*MyPool));
    MyPool->magic = ASIO_PROTOCOL_MAGIC;
    MyPool->targetPid = PID;
    MyPool->startupDebug = startupSettings.debug;
    char diagnosticsSetting[2] = {};
    environmentDiagnostics = GetEnvironmentVariableA("ASIO_DIAGNOSTICS", diagnosticsSetting, sizeof(diagnosticsSetting)) == 1 && diagnosticsSetting[0] == '1';
    MyPool->diagnosticsEnabled = environmentDiagnostics || startupSettings.debug;
    asio::WakeEvent wakeEvent;
    wakeEvent.create(PID); // Missing event is a supported polling fallback.
    backendWake = wakeEvent.get();
    InterlockedExchange(&MyPool->heartbeat, LONG(GetTickCount()));
    FMOD_DSP *leaseDSP = nullptr;
    FMOD_CHANNELGROUP *leaseMaster = nullptr;
    FMOD_RESULT leaseResult = attachAudioLease(fmodSystem, MyPool, &leaseDSP, &leaseMaster);
    if (leaseResult != FMOD_OK)
    {
        puts(ui::text("backend.lease_failed",{FMOD_ErrorString(leaseResult)}).c_str());
        FMOD_System_Release(fmodSystem);
        UnmapViewOfFile(lpBase); CloseHandle(hMapFile);
        return 1;
    }
    InterlockedExchange(&MyPool->backendReady, 1);
    auto closeBackend = [&]()
    {
        InterlockedExchange(&MyPool->backendReady, 0);
        FMOD_ChannelGroup_RemoveDSP(leaseMaster, leaseDSP);
        FMOD_DSP_Release(leaseDSP);
        FMOD_System_Release(fmodSystem);
        fmodSystem = nullptr;
        UnmapViewOfFile(lpBase); CloseHandle(hMapFile);
    };

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, PID);
    if (!hProcess)
    {
        fprintf(stderr,"%s\n",ui::text("backend.access_failed",{std::to_string(PID),std::to_string(GetLastError())}).c_str());
        closeBackend();
        return 1;
    }

    AsioLiveMapping live;
    if (liveAttach && !live.create(PID,startupSettings.contractMode)) {
        fprintf(stderr,"%s\n",ui::text("backend.session_failed").c_str());
        CloseHandle(hProcess); closeBackend(); return 1;
    }
    wchar_t executable[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length >= MAX_PATH) { CloseHandle(hProcess); closeBackend(); return 1; }
    std::wstring dllPath = executable;
    dllPath.resize(dllPath.find_last_of(L'\\') + 1);
    dllPath += L"osu!asio_sound.dll";

    if (!DllInject(hProcess, dllPath.c_str()))
    {
        puts(ui::text("backend.inject_failed").c_str());
        CloseHandle(hProcess); closeBackend();
        return 1;
    }

    if (liveAttach) {
        DWORD deadline = GetTickCount() + 45000;
        while (InterlockedCompareExchange(&live.state->status, 0, 0) >= 0 &&
               InterlockedCompareExchange(&live.state->status, 0, 0) < 2 &&
               LONG(deadline - GetTickCount()) > 0 && WaitForSingleObject(hProcess, 0) == WAIT_TIMEOUT) {
            InterlockedExchange(&MyPool->heartbeat, LONG(GetTickCount()));
            Sleep(10);
        }
        if (InterlockedCompareExchange(&live.state->status, 0, 0) != 2) {
            char errorCode[32];snprintf(errorCode,sizeof(errorCode),"0x%08lx",live.state->error);
            fprintf(stderr,"%s\n",ui::text("backend.adapter_failed",{errorCode}).c_str());
            CloseHandle(hProcess); closeBackend(); return 1;
        }
        LONG contract=InterlockedCompareExchange(&live.state->contractStatus,0,0);
        if(contract!=1&&contract!=2){fprintf(stderr,"%s\n",ui::text("contract.failed").c_str());CloseHandle(hProcess);closeBackend();return 1;}
        printf("%s %s\n",contract==1?"[Verified Contract]":"[Compat/Adaptive]",ui::text(contract==1?"contract.verified":"contract.adaptive").c_str());
        printf("[ASIO_READY] %s\n",ui::text("backend.ready",{std::to_string(PID)}).c_str());
    }

    playbackLog=new(std::nothrow) asio::AsyncTextLog;
    if(!playbackLog||!playbackLog->start(stdout)){fprintf(stderr,"%s\n",ui::text("backend.logger_failed").c_str());CloseHandle(hProcess);closeBackend();return 1;}
    settingsStopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!settingsStopEvent){CloseHandle(hProcess);closeBackend();return 1;}
    settingsWatcherRunning=true;
    HANDLE settingsThread=CreateThread(nullptr,0,watchSettings,nullptr,0,nullptr);
    if(!settingsThread){settingsWatcherRunning=false;CloseHandle(settingsStopEvent);CloseHandle(hProcess);closeBackend();return 1;}
    Work = true;
    HANDLE thread = CreateThread(0, 0, mainloop, 0, 0, 0);
    if (!thread) {
        settingsWatcherRunning=false;SetEvent(settingsStopEvent);WaitForSingleObject(settingsThread,INFINITE);
        CloseHandle(settingsThread);CloseHandle(settingsStopEvent);CloseHandle(hProcess);closeBackend();return 1;
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);

    MSG msg;
    bool running = true;
    while (running)
    {
        DWORD wait = MsgWaitForMultipleObjects(1, &hProcess, FALSE, 1000, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    Work = false;
    settingsWatcherRunning=false;
    SetEvent(settingsStopEvent);
    WaitForSingleObject(thread, INFINITE);
    WaitForSingleObject(settingsThread,INFINITE);
    CloseHandle(thread);
    CloseHandle(settingsThread);
    CloseHandle(settingsStopEvent);settingsStopEvent=nullptr;

    closeBackend();
    if(playbackLog) {
        playbackLog->write("FMOD System released. Diagnostic lines dropped=%lu\n",playbackLog->dropped());
        playbackLog->stop();
    }
    CloseHandle(hProcess);
    return backendFault ? 1 : 0;
}
