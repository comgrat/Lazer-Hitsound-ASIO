// Independent regression fixture. The /I target is selected by Test-OutputRegression.ps1.
// Only NOSOUND is initialized; the renamed production main is never called.
#define main AsioOutputRegressionProductionMain
#include "osu!asio_sound.cpp"
#undef main
#include <cmath>
#include <memory>
#include <stdexcept>
#include <functional>
#include <algorithm>

namespace regression {
static int checks = 0, failures = 0;
static unsigned stage = 0;
static std::wstring artifacts;

static void check(bool pass, const char *name) {
    ++checks;
    if (!pass) ++failures;
    printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
}
static void require(bool pass, const char *name) {
    check(pass, name);
    if (!pass) throw std::runtime_error(name);
}
static void section(const char *name) { printf("\nSTAGE %u: %s\n", ++stage, name); }
static bool closeTo(float value, float expected, float tolerance = 0.00001f) {
    return std::isfinite(value) && std::fabs(value - expected) <= tolerance;
}
static bool eventually(const std::function<bool()> &condition, DWORD timeout = 2500) {
    const ULONGLONG start = GetTickCount64();
    do { if (condition()) return true; Sleep(5); } while (GetTickCount64() - start < timeout);
    return condition();
}
static void pump(DWORD milliseconds) {
    const ULONGLONG start = GetTickCount64();
    do { FMOD_System_Update(fmodSystem); Sleep(5); } while (GetTickCount64() - start < milliseconds);
}
static std::string ini(int percent, bool decorated = false, int debug = 0) {
    if (decorated) return std::string("\xef\xbb\xbf") +
        "# unrelated 98765\r\n[Config]\r\nDebug Log = " + std::to_string(debug) +
        " ; 777\r\nUnknown Number = 12000\r\nOutput Volume = " + std::to_string(percent) +
        " # volume 42\r\nSample Rate = 48000\r\n; 12345\r\nASIO Driver ID = 0\r\nBuffer Size = 16\r\n";
    return "[Config]\nBuffer Size = 16\nASIO Driver ID = 0\nSample Rate = 48000\nOutput Volume = " +
        std::to_string(percent) + "\nDebug Log = " + std::to_string(debug) + "\n";
}
static void writeBytes(const std::wstring &file, const std::string &text) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ULONGLONG deadline = GetTickCount64() + 500;
    do {
        handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    require(handle != INVALID_HANDLE_VALUE, "create only the fixture INI");
    DWORD bytes = 0;
    bool ok = WriteFile(handle, text.data(), DWORD(text.size()), &bytes, nullptr) && bytes == text.size();
    FlushFileBuffers(handle); CloseHandle(handle);
    require(ok, "write complete fixture bytes");
}
static std::string readBytes(const std::wstring &file) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(handle, nullptr), bytes = 0;
    std::string data(size, '\0');
    bool ok = ReadFile(handle, data.data(), size, &bytes, nullptr) && bytes == size;
    CloseHandle(handle); return ok ? data : std::string{};
}

struct System {
    std::unique_ptr<sharepool> pool;
    System() {
        require(!fmodSystem, "previous FMOD system has been released");
        require(FMOD_System_Create(&fmodSystem, FMOD_VERSION) == FMOD_OK, "create real FMOD system");
        require(FMOD_System_SetOutput(fmodSystem, FMOD_OUTPUTTYPE_NOSOUND) == FMOD_OK, "select NOSOUND before init");
        require(FMOD_System_SetDSPBufferSize(fmodSystem, 128, 2) == FMOD_OK, "set deterministic software buffer");
        require(FMOD_System_SetSoftwareFormat(fmodSystem, 48000, FMOD_SPEAKERMODE_STEREO, 0) == FMOD_OK, "stereo software format");
        require(FMOD_System_Init(fmodSystem, 32, FMOD_INIT_NORMAL, nullptr) == FMOD_OK, "initialize without opening an audio device");
        FMOD_OUTPUTTYPE actual;
        require(FMOD_System_GetOutput(fmodSystem, &actual) == FMOD_OK && actual == FMOD_OUTPUTTYPE_NOSOUND, "FMOD confirms NOSOUND output");
        require(FMOD_System_GetMasterChannelGroup(fmodSystem, &asioOutputGroup) == FMOD_OK, "use real master as ASIO output group");
        outputVolume = 100; requestedOutputVolume = 100; requestedDebugLog = 0; debugLogEnabled = 0;
        require(FMOD_ChannelGroup_SetVolume(asioOutputGroup, outputVolume / 100.0f) == FMOD_OK, "initialize master percent exactly as production main");
        pool = std::make_unique<sharepool>();
        MyPool = pool.get(); MyPool->magic = ASIO_PROTOCOL_MAGIC; MyPool->backendReady = 1;
        playbackStats = {}; nextVoiceId = 0; backendFault = false; Work = false;
        mainThreadId = GetCurrentThreadId();
        MSG message = {}; PeekMessage(&message, nullptr, 0, 0, PM_NOREMOVE);
        while (PeekMessage(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    }
    ~System() {
        Work = false;
        if (fmodSystem) FMOD_System_Release(fmodSystem);
        fmodSystem = nullptr; asioOutputGroup = nullptr; MyPool = nullptr;
    }
};

struct MappedSample {
    SampleLoadedPayload payload = {};
    HANDLE mapping = nullptr;
    float *pcm = nullptr;
    MappedSample(HSAMPLE handle, DWORD generation, DWORD frames = 48000) {
        payload.sample = handle; payload.generation = generation; payload.sampleRate = 48000;
        payload.channels = 1; payload.format = SAMPLE_FORMAT_FLOAT;
        payload.dataLength = frames * sizeof(float);
        sprintf_s(payload.mappingName, "Local\\TOutputRegression_%lu_%lu_%lu", GetCurrentProcessId(), handle, generation);
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, payload.dataLength, payload.mappingName);
        require(mapping && GetLastError() != ERROR_ALREADY_EXISTS, "create isolated test PCM mapping");
        pcm = static_cast<float *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, payload.dataLength));
        require(pcm != nullptr, "map test PCM");
        for (DWORD i = 0; i < frames; ++i) pcm[i] = 0.1f * std::sin(float(i) * 0.0576f);
        auto &ack = MyPool->sampleAcks[handle % SAMPLE_ACK_COUNT];
        ack.sample = LONG(handle); ack.requestedGeneration = LONG(generation); ack.readyGeneration = 0;
    }
    ~MappedSample() { if (pcm) UnmapViewOfFile(pcm); if (mapping) CloseHandle(mapping); }
};

struct Watcher {
    HANDLE thread = nullptr;
    Watcher(const std::wstring &file) {
        configPath = file;
        settingsStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(settingsStopEvent != nullptr, "create real watcher stop event");
        settingsWatcherRunning = true;
        thread = CreateThread(nullptr, 0, watchSettings, nullptr, 0, nullptr);
        require(thread != nullptr, "start production watchSettings thread");
    }
    ~Watcher() { stop(); }
    void stop() {
        if (!thread) return;
        settingsWatcherRunning = false; SetEvent(settingsStopEvent);
        DWORD waited = WaitForSingleObject(thread, 3000);
        check(waited == WAIT_OBJECT_0, "settings watcher joins after event");
        if (waited != WAIT_OBJECT_0) WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread); CloseHandle(settingsStopEvent);
        thread = nullptr; settingsStopEvent = nullptr;
    }
};

struct Worker {
    HANDLE thread = nullptr;
    Worker() {
        backendWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(backendWake != nullptr, "create production worker wake event");
        Work = true;
        thread = CreateThread(nullptr, 0, mainloop, nullptr, 0, nullptr);
        require(thread != nullptr, "start real production event worker");
    }
    ~Worker() { stop(); }
    void send(const Event &event) {
        LONG tail = InterlockedCompareExchange(&MyPool->tail, 0, 0);
        LONG next = (tail + 1) % EVENT_POOL_SIZE;
        require(next != InterlockedCompareExchange(&MyPool->head, 0, 0), "test queue has capacity");
        MyPool->events[tail] = event; MemoryBarrier(); InterlockedExchange(&MyPool->tail, next);
        SetEvent(backendWake);
    }
    void drained() {
        require(eventually([] { return InterlockedCompareExchange(&MyPool->head, 0, 0) == InterlockedCompareExchange(&MyPool->tail, 0, 0); }), "worker consumes queued test events");
    }
    void stop() {
        if (!thread) return;
        Work = false;
        DWORD waited = WaitForSingleObject(thread, 3000);
        check(waited == WAIT_OBJECT_0, "event worker joins within bound");
        if (waited != WAIT_OBJECT_0) WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread); thread = nullptr;
        CloseHandle(backendWake);
        backendWake = nullptr;
    }
};
static Event state(HCHANNEL handle, DWORD generation, HSAMPLE sample, bool play, bool loop = false) {
    Event event = {}; event.type = EVENT_CHANNEL_STATE; event.generation = generation;
    auto &p = event.data.channelState; p.channel = handle; p.sample = sample;
    p.state = play ? CHANNEL_STATE_PLAYING : CHANNEL_STATE_PAUSED; p.volume = 0.25f; p.looping = loop ? 1 : 0;
    return event;
}
static Event created(HCHANNEL handle, DWORD generation, HSAMPLE sample) {
    Event event = {}; event.type = EVENT_CHANNEL_CREATED; event.generation = generation;
    event.data.channelCreated.channel = handle; event.data.channelCreated.sample = sample; return event;
}
static Event removed(HCHANNEL handle, DWORD generation) {
    Event event = {}; event.type = EVENT_CHANNEL_REMOVED; event.generation = generation;
    event.data.channelRemoved.channel = handle; return event;
}
static int activeChannels() { int count = -1; FMOD_System_GetChannelsPlaying(fmodSystem, &count, nullptr); return count; }

static void parser() {
    section("keyed parser: UTF-8 BOM, reorder, comments, no scanning of unrelated numbers");
    asio::Settings settings; std::string error;
    check(asio::parseSettings(ini(25, true), settings, error) && settings.buffer == 16 && settings.driver == 0 &&
        settings.rate == 48000 && settings.volume == 25 && settings.debug == 0 && settings.contractMode == 1, "BOM/reordered legacy five keys default to Adaptive");
    check(asio::parseSettings(ini(25, true) + "Contract Mode = 0\n", settings, error) && settings.contractMode == 0,
        "optional Contract Mode selects Strict");
    const std::vector<std::string> invalid = {
        "[Config]\nBuffer Size=16\nASIO Driver ID=0\nSample Rate=48000\nDebug Log=0\n# Output Volume missing; 25 100 1000\n",
        ini(101), ini(-1), ini(25) + "Output Volume = 50\n",
        ini(25) + "Contract Mode = 2\n", ini(25) + "Contract Mode = 0\nContract Mode = 1\n",
        "[Config]\nBuffer Size=16\nASIO Driver ID=0\nSample Rate=48000\nOutput Volume=25abc\nDebug Log=0\n",
        "[Config]\nBuffer Size=16\nASIO Driver ID=0\nSample Rate=48000\nOutput Volume=\nDebug Log=0\n",
        std::string("[\0C\0o\0n\0f\0i\0g\0]", 15)
    };
    for (size_t i = 0; i < invalid.size(); ++i) {
        settings.volume = 37;
        const bool parsed = asio::parseSettings(invalid[i], settings, error);
        char name[160]; sprintf_s(name, "invalid config case %zu rejected without altering prior settings", i + 1);
        check(!parsed && settings.volume == 37 && !error.empty(), name);
    }
    const auto file = artifacts + L"\\配置-按键.ini";
    writeBytes(file, ini(25, true));
    require(asio::loadSettings(file, settings, error), "loadSettings reads actual absolute Unicode path");
    check(settings.volume == 25, "Unicode fixture yields requested output volume");
}

static void helpersAndVolume() {
    section("real volume levels and legitimate voices sharing one sample");
    System system;
    MappedSample mapped(17, 3, 48000 * 3);
    unordered_map<HSAMPLE, SampleData> samples;
    require(loadSampleData(mapped.payload, samples), "production PCM mapping/ACK load");
    float firstPcm = mapped.pcm[1];
    mapped.pcm[1] = 0.9f;
    void *fmodPcm = nullptr, *secondPcm = nullptr;
    unsigned int firstBytes = 0, secondBytes = 0;
    require(FMOD_Sound_Lock(samples.at(17).sound, 0, 8, &fmodPcm, &secondPcm, &firstBytes, &secondBytes) == FMOD_OK,
        "lock FMOD-owned copied PCM after source mapping changed");
    check(firstBytes >= 8 && closeTo(static_cast<float *>(fmodPcm)[1], firstPcm),
        "CREATESAMPLE retains its own PCM copy after mapped source mutation");
    FMOD_Sound_Unlock(samples.at(17).sound, fmodPcm, secondPcm, firstBytes, secondBytes);
    ChannelData first; first.sample = 17; first.generation = 10; first.handle = 100; first.volume = 0.25f;
    ensurePaused(first);
    check(first.phase == ChannelData::Phase::Fresh && !first.channel, "pause before first PLAY remains fresh");
    ensurePlaying(first, samples.at(17));
    require(first.channel && first.phase == ChannelData::Phase::Playing, "create real first production voice");
    FMOD_CHANNEL *original = first.channel; const auto voiceId = first.voiceId;
    const int percentages[] = {0, 1, 25, 50, 100};
    for (int percent : percentages) {
        requestedOutputVolume = percent;
        require(refreshOutputVolume(), "worker refreshes ASIO master volume");
        float channelGain = -1, groupGain = -1;
        FMOD_CHANNELGROUP *actualGroup = nullptr;
        bool ok = FMOD_Channel_GetVolume(first.channel, &channelGain) == FMOD_OK &&
            FMOD_Channel_GetChannelGroup(first.channel, &actualGroup) == FMOD_OK && actualGroup == asioOutputGroup &&
            FMOD_ChannelGroup_GetVolume(actualGroup, &groupGain) == FMOD_OK;
        printf("MEASURE volume: percent=%d channel=%.7f group=%.7f product=%.7f expected=%.7f voice=%llu\n",
            percent, channelGain, groupGain, channelGain * groupGain, 0.25f * percent / 100.0f, first.voiceId);
        char name[180]; sprintf_s(name, "sample0.25 x master%d%% once via real getters", percent);
        check(ok && closeTo(channelGain, 0.25f) && closeTo(groupGain, percent / 100.0f) &&
            closeTo(channelGain * groupGain, 0.25f * percent / 100.0f), name);
    }
    const auto startsBefore = playbackStats.starts;
    for (int i = 0; i < 8; ++i) ensurePlaying(first, samples.at(17));
    check(first.channel == original && first.voiceId == voiceId && nextVoiceId == 1 && playbackStats.starts == startsBefore,
        "repeated PLAY while playing creates exactly one voice ID");
    ChannelData second; second.sample = 17; second.generation = 11; second.handle = 101; second.volume = 0.25f;
    ensurePlaying(second, samples.at(17)); pump(60);
    FMOD_BOOL a = false, b = false;
    check(second.channel && second.channel != first.channel && second.voiceId != first.voiceId && nextVoiceId == 2 &&
        FMOD_Channel_IsPlaying(first.channel, &a) == FMOD_OK && a && FMOD_Channel_IsPlaying(second.channel, &b) == FMOD_OK && b,
        "two different legal channels sharing sample both remain playing");

    section("pause/resume preserves real pointer and PCM position without seek0");
    unsigned int beforePause = 0, pausedAt = 0, stillPausedAt = 0, resumedAt = 0, later = 0;
    require(FMOD_Channel_GetPosition(first.channel, &beforePause, FMOD_TIMEUNIT_PCM) == FMOD_OK && beforePause > 0, "voice advanced before pause");
    ensurePaused(first); pump(35);
    require(FMOD_Channel_GetPosition(first.channel, &pausedAt, FMOD_TIMEUNIT_PCM) == FMOD_OK, "read paused position");
    ensurePaused(first); pump(45);
    require(FMOD_Channel_GetPosition(first.channel, &stillPausedAt, FMOD_TIMEUNIT_PCM) == FMOD_OK, "read repeated-pause position");
    FMOD_BOOL paused = false;
    check(FMOD_Channel_GetPaused(first.channel, &paused) == FMOD_OK && paused && pausedAt == stillPausedAt,
        "repeated pause is idempotent and does not advance or seek");
    ensurePlaying(first, samples.at(17));
    require(FMOD_Channel_GetPosition(first.channel, &resumedAt, FMOD_TIMEUNIT_PCM) == FMOD_OK, "read resumed position");
    pump(45); FMOD_Channel_GetPosition(first.channel, &later, FMOD_TIMEUNIT_PCM);
    printf("MEASURE positions: before=%u paused=%u repeatPause=%u resumed=%u later=%u pointer=%p voice=%llu\n",
        beforePause, pausedAt, stillPausedAt, resumedAt, later, static_cast<void *>(first.channel), first.voiceId);
    check(first.channel == original && first.voiceId == voiceId && resumedAt >= pausedAt && pausedAt > 0 && later > resumedAt && nextVoiceId == 2,
        "resume uses same FMOD pointer/voice ID and continues nonzero position");
    FMOD_Channel_Stop(first.channel); FMOD_Channel_Stop(second.channel);
    require(FMOD_Sound_Release(samples.at(17).sound) == FMOD_OK, "release helper sample");
}

static void naturallyFinishedHelpers() {
    section("natural completion then property/pause/late PLAY cannot revive same voice");
    System system; MappedSample mapped(19, 4, 2400);
    unordered_map<HSAMPLE, SampleData> samples;
    require(loadSampleData(mapped.payload, samples), "load short completion sample");
    ChannelData voice; voice.sample = 19; voice.generation = 12; voice.handle = 120; voice.volume = 0.25f;
    ensurePlaying(voice, samples.at(19));
    require(voice.channel != nullptr, "short voice created");
    FMOD_CHANNEL *old = voice.channel; const auto id = voice.voiceId;
    bool finished = eventually([&] { FMOD_System_Update(fmodSystem); FMOD_BOOL active = false;
        auto result = FMOD_Channel_IsPlaying(old, &active); return result == FMOD_ERR_INVALID_HANDLE || result == FMOD_ERR_CHANNEL_STOLEN || (result == FMOD_OK && !active); });
    check(finished, "real short FMOD sound naturally completes");
    voice.volume = 0.4f;
    check(!applyChannelState(voice) && voice.phase == ChannelData::Phase::Finished && !voice.channel && !voice.playing,
        "late property update marks completed handle Finished");
    ensurePaused(voice); ensurePlaying(voice, samples.at(19));
    check(!voice.channel && voice.voiceId == id && nextVoiceId == 1,
        "Finished helper rejects late PLAY after update and pause");
    check(eventually([] { FMOD_System_Update(fmodSystem); return activeChannels() == 0; }),
        "FMOD activity count converges after natural completion");
    FMOD_Channel_Stop(old); // An already-invalid handle must not allocate another voice.
    ensurePlaying(voice, samples.at(19));
    check(nextVoiceId == 1 && !voice.channel, "late stop followed by PLAY does not revive Finished data");
    unordered_map<HCHANNEL, ChannelData> channels;
    GenerationHistory generations;
    channels.emplace(120, voice);
    generations.accept(120, 12, true);
    ChannelData fresh; fresh.handle=121; fresh.generation=13;
    channels.emplace(121, fresh);
    retireFinishedChannels(channels, generations);
    check(channels.size()==1 && channels.count(121)==1 && generations.retired(120) && generations.value(120)==12,
        "finished channel data is reclaimed while fresh channels and replay guards remain");
    FMOD_Sound_Release(samples.at(19).sound);
}

static void watching() {
    section("real watcher and event worker apply 0 -> 25 -> 100 -> 0 to an existing voice");
    System system; MappedSample mapped(23, 5, 48000);
    const auto file = artifacts + L"\\配置-实时.ini";
    writeBytes(file, ini(0, true));
    outputVolume = 0; requestedOutputVolume = 0;
    require(FMOD_ChannelGroup_SetVolume(asioOutputGroup, 0) == FMOD_OK, "initialize watcher test master to zero");
    {
        Watcher watcher(file); Worker worker;
        Event load = {}; load.type = EVENT_SAMPLE_LOADED; load.data.sampleLoaded = mapped.payload;
        worker.send(load); worker.send(created(200, 20, 23)); worker.send(state(200, 20, 23, true, true)); worker.drained();
        require(eventually([] { return activeChannels() == 1; }), "looping voice is active before config updates");
        for (int percent : {0, 25, 100, 0}) {
            const std::string text = ini(percent, true, percent == 25 ? 1 : 0);
            writeBytes(file, text);
            bool reached = eventually([&] { float value = -1;
                return requestedOutputVolume.load(std::memory_order_acquire) == percent &&
                    FMOD_ChannelGroup_GetVolume(asioOutputGroup, &value) == FMOD_OK && closeTo(value, percent / 100.0f); });
            float gain = -1; FMOD_ChannelGroup_GetVolume(asioOutputGroup, &gain);
            printf("MEASURE watcher: requested=%d atomic=%d group=%.5f activeChannels=%d\n", percent,
                requestedOutputVolume.load(), gain, activeChannels());
            check(reached && activeChannels() == 1, "real watcher changes master while existing voice stays allocated");
            check(readBytes(file) == text, "configuration readers never rewrite the user's bytes");
        }
        section("invalid, missing-field, partial and deleted INI preserve last valid master");
        writeBytes(file, ini(25));
        require(eventually([] { float value = -1; return requestedOutputVolume.load() == 25 &&
            FMOD_ChannelGroup_GetVolume(asioOutputGroup, &value) == FMOD_OK && closeTo(value, 0.25f); }), "establish last valid 25 percent");
        const std::vector<std::string> invalid = { ini(101),
            "[Config]\nBuffer Size=16\nASIO Driver ID=0\nSample Rate=48000\nDebug Log=0\n; unrelated 100 25000\n",
            "[Config]\nBuffer Size=16\nASIO Driver ID=0\nSample Rate=48000\nOutput Volume=", "" };
        for (size_t i = 0; i < invalid.size(); ++i) {
            writeBytes(file, invalid[i]); Sleep(560);
            float value = -1;
            bool ok = FMOD_ChannelGroup_GetVolume(asioOutputGroup, &value) == FMOD_OK;
            printf("MEASURE invalid: case=%zu atomic=%d group=%.5f\n", i + 1, requestedOutputVolume.load(), value);
            check(ok && requestedOutputVolume.load() == 25 && closeTo(value, 0.25f) && activeChannels() == 1,
                "invalid/partial INI retains previous valid volume and voice");
            check(readBytes(file) == invalid[i], "invalid INI is left unchanged");
        }
        // The path is an explicit test-owned file below the supplied artifacts directory.
        require(file == artifacts + L"\\配置-实时.ini" && DeleteFileW(file.c_str()), "remove only explicit fixture INI to test missing file");
        Sleep(560); float value = -1; FMOD_ChannelGroup_GetVolume(asioOutputGroup, &value);
        check(requestedOutputVolume.load() == 25 && closeTo(value, 0.25f), "missing INI preserves last valid target");
        writeBytes(file, ini(100, true));
        require(eventually([] { float v = -1; return requestedOutputVolume.load() == 100 &&
            FMOD_ChannelGroup_GetVolume(asioOutputGroup, &v) == FMOD_OK && closeTo(v, 1.0f); }), "valid save recovers after invalid/missing files");
        watcher.stop(); worker.stop();
    }
    check(nextVoiceId == 1 && playbackStats.starts == 1, "watcher updates never restart or reallocate the existing loop");
    check(activeChannels() == 0, "event-worker exit stops the existing looping voice");
}

static void workerFinishAndLateEvents() {
    section("production worker sweeps natural completion before late same-generation PLAY");
    System system; MappedSample mapped(29, 6, 2400);
    {
        Worker worker;
        Event load = {}; load.type = EVENT_SAMPLE_LOADED; load.data.sampleLoaded = mapped.payload;
        worker.send(load); worker.send(created(300, 30, 29)); worker.send(state(300, 30, 29, true)); worker.drained();
        require(eventually([] { return activeChannels() == 0; }, 1500), "queued short voice completes naturally");
        Sleep(150);
        Event update = {}; update.type = EVENT_CHANNEL_UPDATED; update.generation = 30;
        update.data.channelUpdated.channel = 300; update.data.channelUpdated.updateMask = CHANNEL_UPDATE_VOLUME;
        update.data.channelUpdated.volume = 0.6f;
        worker.send(update); worker.send(state(300, 30, 29, true)); worker.send(state(300, 30, 29, false));
        // Also exercise the sample-LOADED compensation branch with a completed channel.
        worker.send(load); worker.send(state(300, 30, 29, true)); worker.drained(); Sleep(150);
        check(activeChannels() == 0, "late update/PLAY/pause/sample-LOADED leave naturally-finished voice silent");
        worker.stop();
    }
    printf("MEASURE finished-worker: voiceAllocations=%llu successfulUnpauses=%llu PLAY=%llu finished=%llu staleRejected=%llu\n",
        nextVoiceId, playbackStats.starts, playbackStats.playRequests, playbackStats.finished, playbackStats.staleRejected);
    check(nextVoiceId == 1 && playbackStats.starts == 1, "natural-finish sweep prevents same-generation reallocation");
    // Four late channel events (update, PLAY, pause, PLAY) target the retired
    // generation; LOADED is not a channel event. Counters work without tracing.
    check(!detailedTraceEnabled && playbackStats.finished == 1 && playbackStats.staleRejected == 4,
        "status counts one natural finish and every rejected late event without detailed tracing");
}

static void removedLatePlay() {
    section("removed generation cannot be revived by a late PLAY event");
    System system; MappedSample mapped(31, 7, 48000);
    {
        Worker worker;
        Event load = {}; load.type = EVENT_SAMPLE_LOADED; load.data.sampleLoaded = mapped.payload;
        worker.send(load); worker.send(created(400, 40, 31)); worker.send(state(400, 40, 31, true, true)); worker.drained();
        require(eventually([] { return activeChannels() == 1; }), "removed test voice first becomes active");
        worker.send(removed(400, 40)); worker.drained();
        require(eventually([] { return activeChannels() == 0; }), "REMOVED stops original looping voice");
        worker.send(state(400, 40, 31, true, true)); worker.drained(); Sleep(80);
        const int after = activeChannels();
        printf("MEASURE removed-late-play: activeAfterLatePLAY=%d\n", after);
        check(after == 0, "late PLAY for explicitly removed generation is rejected");
        worker.stop();
    }
    check(nextVoiceId == 1, "removed generation keeps a single lifetime voice allocation");
}

static void reusedGenerationLatePlay() {
    section("old generation cannot replace a newer live generation on the same handle");
    System system; MappedSample mapped(41, 9, 48000);
    {
        Worker worker;
        Event load = {}; load.type = EVENT_SAMPLE_LOADED; load.data.sampleLoaded = mapped.payload;
        worker.send(load); worker.send(created(600, 60, 41)); worker.send(state(600, 60, 41, true, true)); worker.drained();
        require(eventually([] { return activeChannels() == 1; }), "old generation first becomes active");
        worker.send(removed(600, 60)); worker.send(created(600, 61, 41));
        worker.send(state(600, 61, 41, true, true)); worker.drained(); Sleep(80);
        require(activeChannels() == 1, "new legal generation replaces old voice on reused handle");
        worker.send(state(600, 60, 41, true, true)); worker.drained(); Sleep(80);
        check(activeChannels() == 1, "stale PLAY cannot remove newer active voice");
        worker.stop();
    }
    printf("MEASURE reused-late-play: voiceAllocations=%llu successfulUnpauses=%llu\n", nextVoiceId, playbackStats.starts);
    check(nextVoiceId == 2 && playbackStats.starts == 2, "stale old-generation PLAY never allocates a third voice");
}

static void idleWakeHeartbeat() {
    section("wake event drains bursts and keeps heartbeat alive while idle");
    System system;
    Worker worker;
    require(eventually([] { return MyPool->heartbeat != 0; }), "idle worker publishes first heartbeat");
    LONG before = InterlockedCompareExchange(&MyPool->heartbeat, 0, 0);
    Sleep(60);
    LONG after = InterlockedCompareExchange(&MyPool->heartbeat, 0, 0);
    check(after != before && DWORD(GetTickCount() - DWORD(after)) < 250, "idle timed waits continue heartbeat and FMOD updates");
    for (int i = 0; i < 32; ++i) {
        Event event = {};
        event.type = EVENT_NONE;
        worker.send(event);
    }
    worker.drained();
    check(MyPool->backendReady == 1, "coalesced auto-reset signals do not lose queued burst");
    queueLatency = {};
    requestedDebugLog = 1;
    require(eventually([] { return MyPool->diagnosticsEnabled == 1; }), "diagnostic mode reaches shared mapping");
    Event play = state(990, 990, 99999, true);
    play.data.channelState.time = asio::qpcMilliseconds();
    worker.send(play);
    worker.drained();
    worker.stop();
    auto delay = queueLatency.summary();
    check(delay.count == 1 && delay.maximum >= 0, "backend collects diagnostic PLAY enqueue-to-handler delay");
    requestedDebugLog = 0;
}

static void loopVoiceLoss() {
    section("loop voice loss is local and preserves other channels");
    System system;
    MappedSample mapped(51, 11, 48000);
    unordered_map<HSAMPLE, SampleData> samples;
    require(loadSampleData(mapped.payload, samples), "load loop-local sample");
    ChannelData voice;
    voice.sample = 51;
    voice.handle = 800;
    voice.generation = 80;
    voice.looping = true;
    ensurePlaying(voice, samples.at(51));
    int priority = -1;
    check(FMOD_Channel_GetPriority(voice.channel, &priority) == FMOD_OK && priority == 0, "loop receives highest FMOD priority");
    // Force actual FMOD channel stealing after deliberately lowering this
    // test voice's priority. Production keeps it at zero.
    FMOD_Channel_SetPriority(voice.channel, 256);
    std::vector<FMOD_CHANNEL *> competing;
    bool stolen = false;
    for (int i = 0; i < 40; ++i)
    {
        FMOD_CHANNEL *other = nullptr;
        FMOD_RESULT result = FMOD_System_PlaySound(fmodSystem, samples.at(51).sound, nullptr, false, &other);
        if (result == FMOD_OK)
        {
            FMOD_Channel_SetMode(other, FMOD_LOOP_NORMAL);
            FMOD_Channel_SetPriority(other, 0);
            competing.push_back(other);
        }
        FMOD_BOOL active = false;
        result = FMOD_Channel_IsPlaying(voice.channel, &active);
        if (result == FMOD_ERR_CHANNEL_STOLEN || result == FMOD_ERR_INVALID_HANDLE)
        {
            stolen = true;
            break;
        }
    }
    require(stolen, "fault injection steals the loop's actual FMOD handle");
    check(!refreshChannelLifetime(voice) && voice.phase == ChannelData::Phase::Finished,
        "stolen loop retires without backend failure");
    check(MyPool->backendReady == 1 && MyPool->failureTail != MyPool->failureHead,
        "loop loss keeps backend alive and publishes local failover");
    auto failure = MyPool->channelFailures[MyPool->failureHead];
    check(failure.channel == 800 && failure.generation == 80, "local failure preserves exact ownership identity");
    for (auto other : competing) FMOD_Channel_Stop(other);
    FMOD_Sound_Release(samples.at(51).sound);
}

static void loopHistoryLimit() {
    section("retired generation history is bounded without replay gaps");
    GenerationHistory history(4);
    check(history.accept(700, 1, true), "long-lived channel starts before watermark advances");
    for (DWORD i = 2; i <= 21; ++i) {
        require(history.accept(1000 + i, i, true), "new generation accepted before retirement");
        history.retire(1000 + i, i);
    }
    check(history.size() <= 5 && history.retirementQueueSize() <= 4, "retired history is bounded in addition to active lifetimes");
    check(history.accept(700, 1, false, true), "tracked live channel remains valid below eviction watermark");
    check(!history.accept(700, 1, false), "untracked stale generation is rejected even with remembered identity");
    check(!history.accept(1002, 2, false), "evicted old PLAY cannot revive voice");
    check(history.accept(1002, 22, false), "new lifetime works after tombstone eviction");
    auto remembered = history.size();
    check(!history.accept(9000, 23, false, false, false) && history.size() == remembered,
        "orphan property events do not accumulate live history entries");
    check(!history.accept(700, 23, false, false, false) && history.value(700) == 1,
        "orphan future update cannot invalidate an active generation");
    System system;
    MappedSample mapped(53, 12, 48000);
    unordered_map<HSAMPLE, SampleData> samples;
    require(loadSampleData(mapped.payload, samples), "load sample for post-eviction playback");
    auto allocations = nextVoiceId;
    if (history.accept(1002, 2, false))
    {
        ChannelData stale;
        stale.handle = 1002;
        stale.generation = 2;
        ensurePlaying(stale, samples.at(53));
    }
    check(nextVoiceId == allocations, "evicted stale PLAY allocates no actual FMOD voice");
    ChannelData fresh;
    fresh.handle = 1002;
    fresh.generation = 22;
    fresh.looping = true;
    ensurePlaying(fresh, samples.at(53));
    check(fresh.channel && fresh.playing && nextVoiceId == allocations + 1,
        "new generation plays an actual FMOD voice after tombstone eviction");
    if (fresh.channel) FMOD_Channel_Stop(fresh.channel);
    FMOD_Sound_Release(samples.at(53).sound);
}

static void failingBackend() {
    section("fault synchronously mutes master, then event-worker shutdown stops loop");
    System system; MappedSample mapped(37, 8, 48000);
    {
        Worker worker;
        Event load = {}; load.type = EVENT_SAMPLE_LOADED; load.data.sampleLoaded = mapped.payload;
        worker.send(load); worker.send(created(500, 50, 37)); worker.send(state(500, 50, 37, true, true)); worker.drained();
        require(eventually([] { return activeChannels() == 1; }), "fault-test loop is initially playing");
        backendFailed(FMOD_ERR_OUTPUT_INIT);
        FMOD_BOOL muted = false;
        check(MyPool->backendReady == 0 && FMOD_ChannelGroup_GetMute(asioOutputGroup, &muted) == FMOD_OK && muted,
            "backendFailed clears readiness and synchronously mutes actual master");
        require(eventually([&] { return WaitForSingleObject(worker.thread, 0) == WAIT_OBJECT_0; }), "failed backend's real worker exits");
        worker.stop();
    }
    check(backendFault && activeChannels() == 0, "permanent failure stops all loop voices before cleanup completes");
    MSG message = {};
    check(PeekMessage(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE, "failed worker posts WM_QUIT to test owner");
}
} // namespace regression

int wmain(int argc, wchar_t **argv) {
    using namespace regression;
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3 || wcscmp(argv[1], L"--artifacts") != 0) return 2;
    artifacts = argv[2];
    if (artifacts.find(L"T_OUTPUT_REGRESSION_RUN_") == std::wstring::npos ||
        artifacts.find(L"..") != std::wstring::npos) return 2;
    printf("OutputRegression: real FMOD NOSOUND; production main never invoked; no acoustic/device validation\n");
    try {
        parser(); helpersAndVolume(); naturallyFinishedHelpers(); watching();
        workerFinishAndLateEvents(); removedLatePlay(); reusedGenerationLatePlay(); idleWakeHeartbeat(); loopVoiceLoss(); loopHistoryLimit(); failingBackend();
    } catch (const std::exception &error) {
        printf("FATAL: %s\n", error.what());
        ++failures;
    }
    printf("RESULT {\"checks\":%d,\"failures\":%d,\"stages\":%u,\"output\":\"FMOD_NOSOUND\",\"acousticValidation\":false}\n",
        checks, failures, stage);
    return failures ? 1 : 0;
}
