// Native-hook mock regression tests.
//
// Scope: the real MyBASS_* detours in ../dllmain.cpp, with mock BASS originals
// and a process-local heap sharepool. This executable deliberately does NOT:
//   - call initDLL, MH_Initialize, MH_CreateHook, or MH_EnableHook;
//   - load bass.dll / bassmix.dll;
//   - start the game, load a chart, or open an audio device.
//
// A production backend is represented only by the shared-memory protocol:
// magic/targetPid/backendReady/heartbeat, sampleAcks, and the event queue.
//
// These are contract tests, not game or acoustic acceptance.

#include "../dllmain.cpp"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <io.h>
#include <thread>
#include <utility>
#include "../settings.h"
#include "../channel_failure.h"

namespace
{
int g_checks = 0;
int g_failures = 0;

struct VolumeWrite
{
    DWORD channel;
    float value;
};

struct MockBass
{
    HSAMPLE sampleHandle = 0x1101;
    HCHANNEL channelHandle = 0x2201;

    bool sampleLoadOk = true;
    bool sampleInfoOk = true;
    bool sampleDataOk = true;
    bool sampleGetChannelOk = true;
    bool playOk = true;
    bool pauseOk = true;
    bool stopOk = true;
    bool setAttributeOk = true;
    bool flagsOk = true;
    bool mixerAddOk = true;
    bool mixerFlagsOk = true;
    bool mixerRemoveOk = true;
    bool streamFreeOk = true;

    int sampleLoadCalls = 0;
    int sampleInfoCalls = 0;
    int sampleDataCalls = 0;
    int sampleGetChannelCalls = 0;
    int playCalls = 0;
    int pauseCalls = 0;
    int stopCalls = 0;
    int isActiveCalls = 0;
    int setAttributeCalls = 0;
    int channelFlagsCalls = 0;
    int mixerAddCalls = 0;
    int mixerFlagsCalls = 0;
    int mixerRemoveCalls = 0;
    int streamFreeCalls = 0;

    DWORD active = 0;
    DWORD mixerFlags = 0;
    DWORD channelFlags = 0;
    std::vector<VolumeWrite> volumeWrites;
    std::unordered_map<DWORD, float> nativeVolume;
};

MockBass g_mock;
sharepool *g_pool = nullptr;

// Deterministic lock probe: while the real ChannelSetAttribute hook is inside
// the original BASS call, another thread must not be able to enter stateLock.
std::atomic<bool> g_probeLockDuringSetAttribute{false};
std::atomic<int> g_probeLockResult{-1}; // 0 = blocked (good), 1 = entered (bad)
std::function<void(DWORD)> g_activeFault;

void fail(const char *file, int line, const std::string &message)
{
    ++g_failures;
    std::printf("    FAIL %s:%d: %s\n", file, line, message.c_str());
}

void check(bool condition, const char *expression, const char *file, int line)
{
    ++g_checks;
    if (!condition)
        fail(file, line, std::string("expected: ") + expression);
}

void checkEqual(long long actual, long long expected, const char *actualText,
                const char *expectedText, const char *file, int line)
{
    ++g_checks;
    if (actual != expected)
    {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "%s == %s (actual %lld, expected %lld)",
                      actualText, expectedText, actual, expected);
        fail(file, line, message);
    }
}

void checkNear(float actual, float expected, float epsilon, const char *actualText,
               const char *expectedText, const char *file, int line)
{
    ++g_checks;
    if (!std::isfinite(actual) || std::fabs(actual - expected) > epsilon)
    {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "%s == %s (actual %.9g, expected %.9g, eps %.9g)",
                      actualText, expectedText, actual, expected, epsilon);
        fail(file, line, message);
    }
}

#define CHECK_TRUE(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define CHECK_FALSE(expression) check(!(expression), "!(" #expression ")", __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, epsilon) \
    checkNear((actual), (expected), (epsilon), #actual, #expected, __FILE__, __LINE__)

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 2000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// ---------------------------------------------------------------------------
// Mock BASS entry points.
// ---------------------------------------------------------------------------

HSAMPLE WINAPI MockBASS_SampleLoad(BOOL, const void *, QWORD, DWORD, DWORD, DWORD)
{
    ++g_mock.sampleLoadCalls;
    return g_mock.sampleLoadOk ? g_mock.sampleHandle : 0;
}

BOOL WINAPI MockBASS_SampleGetInfo(HSAMPLE, BASS_SAMPLE *info)
{
    ++g_mock.sampleInfoCalls;
    if (!g_mock.sampleInfoOk || info == nullptr)
        return FALSE;
    std::memset(info, 0, sizeof(*info));
    info->Frequency = 48000;
    info->Chans = 1;
    info->Length = 64;
    info->Flags = 0;
    return TRUE;
}

BOOL WINAPI MockBASS_SampleGetData(HSAMPLE, void *buffer)
{
    ++g_mock.sampleDataCalls;
    if (!g_mock.sampleDataOk || buffer == nullptr)
        return FALSE;
    std::memset(buffer, 0, 64);
    return TRUE;
}

BOOL WINAPI MockBASS_SampleFree(HSAMPLE)
{
    return TRUE;
}

HCHANNEL WINAPI MockBASS_SampleGetChannel(HSAMPLE, DWORD)
{
    ++g_mock.sampleGetChannelCalls;
    return g_mock.sampleGetChannelOk ? g_mock.channelHandle : 0;
}

BOOL WINAPI MockBASS_ChannelPlay(DWORD, BOOL)
{
    ++g_mock.playCalls;
    if (!g_mock.playOk)
        return FALSE;
    g_mock.active = 1;
    return TRUE;
}

BOOL WINAPI MockBASS_ChannelPause(DWORD)
{
    ++g_mock.pauseCalls;
    if (!g_mock.pauseOk)
        return FALSE;
    g_mock.active = 0;
    return TRUE;
}

BOOL WINAPI MockBASS_ChannelStop(DWORD)
{
    ++g_mock.stopCalls;
    if (!g_mock.stopOk)
        return FALSE;
    g_mock.active = 0;
    return TRUE;
}

DWORD WINAPI MockBASS_ChannelIsActive(DWORD handle)
{
    ++g_mock.isActiveCalls;
    if (g_activeFault)
    {
        auto fault = std::move(g_activeFault);
        g_activeFault = {};
        fault(handle);
    }
    return g_mock.active;
}

BOOL WINAPI MockBASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value)
{
    ++g_mock.setAttributeCalls;
    if (g_probeLockDuringSetAttribute.exchange(false))
    {
        std::thread probe([] {
            if (TryEnterCriticalSection(&stateLock))
            {
                g_probeLockResult = 1;
                LeaveCriticalSection(&stateLock);
            }
            else
            {
                g_probeLockResult = 0;
            }
        });
        probe.join();
    }
    if (!g_mock.setAttributeOk)
        return FALSE;
    if (attrib == BASS_ATTRIB_VOL)
    {
        g_mock.volumeWrites.push_back({handle, value});
        g_mock.nativeVolume[handle] = value;
    }
    return TRUE;
}

DWORD WINAPI MockBASS_ChannelFlags(DWORD, DWORD flags, DWORD mask)
{
    ++g_mock.channelFlagsCalls;
    if (!g_mock.flagsOk)
        return DWORD(-1);
    g_mock.channelFlags = (g_mock.channelFlags & ~mask) | (flags & mask);
    return g_mock.channelFlags;
}

BOOL WINAPI MockBASS_Mixer_StreamAddChannel(DWORD, DWORD, DWORD flags)
{
    ++g_mock.mixerAddCalls;
    if (!g_mock.mixerAddOk)
        return FALSE;
    g_mock.mixerFlags = flags;
    return TRUE;
}

DWORD WINAPI MockBASS_Mixer_ChannelFlags(DWORD, DWORD flags, DWORD mask)
{
    ++g_mock.mixerFlagsCalls;
    if (!g_mock.mixerFlagsOk)
        return DWORD(-1);
    g_mock.mixerFlags = (g_mock.mixerFlags & ~mask) | (flags & mask);
    return g_mock.mixerFlags;
}

BOOL WINAPI MockBASS_Mixer_ChannelRemove(DWORD)
{
    ++g_mock.mixerRemoveCalls;
    if (!g_mock.mixerRemoveOk)
        return FALSE;
    g_mock.mixerFlags = 0;
    return TRUE;
}

BOOL WINAPI MockBASS_StreamFree(DWORD)
{
    ++g_mock.streamFreeCalls;
    return g_mock.streamFreeOk ? TRUE : FALSE;
}

void installMocks()
{
    pOrigBASS_SampleLoad = &MockBASS_SampleLoad;
    pOrigBASS_SampleGetInfo = &MockBASS_SampleGetInfo;
    pOrigBASS_SampleGetData = &MockBASS_SampleGetData;
    pOrigBASS_SampleFree = &MockBASS_SampleFree;
    pOrigBASS_SampleGetChannel = &MockBASS_SampleGetChannel;
    pOrigBASS_ChannelPlay = &MockBASS_ChannelPlay;
    pOrigBASS_ChannelPause = &MockBASS_ChannelPause;
    pOrigBASS_ChannelStop = &MockBASS_ChannelStop;
    pOrigBASS_ChannelIsActive = &MockBASS_ChannelIsActive;
    pOrigBASS_ChannelSetAttribute = &MockBASS_ChannelSetAttribute;
    pOrigBASS_ChannelFlags = &MockBASS_ChannelFlags;
    pOrigBASS_Mixer_StreamAddChannel = &MockBASS_Mixer_StreamAddChannel;
    pOrigBASS_Mixer_ChannelFlags = &MockBASS_Mixer_ChannelFlags;
    pOrigBASS_Mixer_ChannelRemove = &MockBASS_Mixer_ChannelRemove;
    pOrigBASS_StreamFree = &MockBASS_StreamFree;
}

// ---------------------------------------------------------------------------
// Fixture helpers. The locks and pool are created by the test, not initDLL.
// ---------------------------------------------------------------------------

void resetFixture()
{
    pollerRunning = false;
    hooksInitialised = true;
    nextGeneration = 0;

    EnterCriticalSection(&stateLock);
    for (auto &entry : sampleInfoMap)
    {
        if (entry.second.mappingHandle != nullptr)
            CloseHandle(entry.second.mappingHandle);
    }
    sampleInfoMap.clear();
    channelInfoMap.clear();
    LeaveCriticalSection(&stateLock);

    std::memset(g_pool, 0, sizeof(*g_pool));
    MyPool = g_pool;
    MyPool->magic = ASIO_PROTOCOL_MAGIC;
    MyPool->targetPid = GetCurrentProcessId();
    InterlockedExchange(&MyPool->backendReady, 1);
    InterlockedExchange(&MyPool->heartbeat, LONG(GetTickCount()));
    MyPool->head = 0;
    MyPool->tail = 0;

    debugLogEnabled = 0;
    traceEnabled = false;
    g_activeFault = {};
    g_mock = MockBass{};
    installMocks();
}

void clearEvents()
{
    MyPool->head = 0;
    MyPool->tail = 0;
}

void clearVolumeWrites()
{
    g_mock.volumeWrites.clear();
}

std::vector<Event> eventsSnapshot()
{
    std::vector<Event> result;
    LONG head = MyPool->head;
    LONG tail = MyPool->tail;
    for (int guard = 0; head != tail && guard < EVENT_POOL_SIZE + 1; ++guard)
    {
        result.push_back(MyPool->events[head]);
        head = (head + 1) % EVENT_POOL_SIZE;
    }
    return result;
}

int countState(std::vector<Event> &events, DWORD state)
{
    int count = 0;
    for (const Event &event : events)
        if (event.type == EVENT_CHANNEL_STATE && event.data.channelState.state == state)
            ++count;
    return count;
}

int countRemoved(std::vector<Event> &events, DWORD generation)
{
    int count = 0;
    for (const Event &event : events)
        if (event.type == EVENT_CHANNEL_REMOVED && event.generation == generation)
            ++count;
    return count;
}

int countCreated(std::vector<Event> &events, DWORD generation)
{
    int count = 0;
    for (const Event &event : events)
        if (event.type == EVENT_CHANNEL_CREATED && event.generation == generation)
            ++count;
    return count;
}

float latestUpdatedVolume(std::vector<Event> &events)
{
    float value = -1.0f;
    for (const Event &event : events)
        if (event.type == EVENT_CHANNEL_UPDATED &&
            (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_VOLUME) != 0)
            value = event.data.channelUpdated.volume;
    return value;
}

bool anyZeroVolume(const std::vector<VolumeWrite> &writes)
{
    return std::any_of(writes.begin(), writes.end(), [](const VolumeWrite &write) {
        return write.value == 0.0f;
    });
}

struct Fixture
{
    HSAMPLE sample = 0;
    HCHANNEL channel = 0;
    DWORD generation = 0;
    DWORD sampleGeneration = 0;
};

Fixture createChannel(bool tagged, float sampleGain, bool acknowledgeFirst = true)
{
    Fixture fixture;
    fixture.sample = MyBASS_SampleLoad(FALSE, nullptr, 0, 0, 0, 0);
    fixture.channel = MyBASS_SampleGetChannel(fixture.sample, 0);
    if (fixture.sample == 0 || fixture.channel == 0)
        return fixture;

    {
        auto it = sampleInfoMap.find(fixture.sample);
        if (it != sampleInfoMap.end())
            fixture.sampleGeneration = it->second.generation;
    }
    if (acknowledgeFirst)
    {
        auto &ack = MyPool->sampleAcks[fixture.sample % SAMPLE_ACK_COUNT];
        InterlockedExchange(&ack.sample, LONG(fixture.sample));
        InterlockedExchange(&ack.requestedGeneration, LONG(fixture.sampleGeneration));
        InterlockedExchange(&ack.readyGeneration, LONG(fixture.sampleGeneration));
    }

    AsioChannelContext(fixture.channel, sampleGain, tagged ? 1u : 0u);
    {
        auto it = channelInfoMap.find(fixture.channel);
        if (it != channelInfoMap.end())
            fixture.generation = it->second.generation;
    }
    return fixture;
}

void setGameVolume(HCHANNEL channel, float volume)
{
    MyBASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, volume);
}

void startExternal(Fixture &fixture, float gameVolume = 1.0f)
{
    setGameVolume(fixture.channel, gameVolume);
    MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, BASS_MIXER_CHAN_PAUSE);
    MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE);
}

bool routeIs(HCHANNEL channel, asio::Route expected)
{
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    bool result = it != channelInfoMap.end() && it->second.routing.route == expected;
    LeaveCriticalSection(&stateLock);
    return result;
}

bool channelPresent(HCHANNEL channel)
{
    EnterCriticalSection(&stateLock);
    bool result = channelInfoMap.find(channel) != channelInfoMap.end();
    LeaveCriticalSection(&stateLock);
    return result;
}

struct ChannelSnapshot
{
    bool present = false;
    bool eligible = false;
    bool suppressNative = false;
    bool restorePending = false;
    float sampleGain = -1.0f;
    float volume = -1.0f;
    DWORD generation = 0;
};

ChannelSnapshot channelSnapshot(HCHANNEL channel)
{
    ChannelSnapshot result;
    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(channel);
    if (it != channelInfoMap.end())
    {
        result.present = true;
        result.eligible = it->second.eligible;
        result.suppressNative = it->second.routing.suppressNative();
        result.restorePending = it->second.restorePending;
        result.sampleGain = it->second.sampleGain;
        result.volume = it->second.volume;
        result.generation = it->second.generation;
    }
    LeaveCriticalSection(&stateLock);
    return result;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

void test_untagged_ui_no_external_play_and_no_mute()
{
    resetFixture();
    Fixture fixture = createChannel(false, 0.25f);
    CHECK_TRUE(fixture.channel != 0);

    setGameVolume(fixture.channel, 1.0f);
    clearEvents();
    clearVolumeWrites();

    CHECK_TRUE(MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, BASS_MIXER_CHAN_PAUSE));
    clearVolumeWrites();
    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE) != DWORD(-1));

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
    CHECK_EQ(g_mock.playCalls, 0);
    CHECK_FALSE(anyZeroVolume(g_mock.volumeWrites));
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(fixture.channel);
    CHECK_TRUE(it != channelInfoMap.end());
    if (it != channelInfoMap.end())
    {
        CHECK_FALSE(it->second.routing.suppressNative());
        CHECK_FALSE(it->second.eligible);
        CHECK_NEAR(it->second.volume, 1.0f, 0.0f);
        CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 1.0f, 0.0f);
    }
    LeaveCriticalSection(&stateLock);
}

void test_tagged_zero_game_volume_still_emits_external_play_native_zero()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    CHECK_TRUE(fixture.channel != 0);

    // Game/user volume 0. The independent chart gain is still 0.25.
    setGameVolume(fixture.channel, 0.0f);
    clearVolumeWrites();

    CHECK_TRUE(MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, BASS_MIXER_CHAN_PAUSE));
    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE) != DWORD(-1));

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 1);
    CHECK_NEAR(latestUpdatedVolume(events), 0.25f, 1.0e-6f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(fixture.channel);
    CHECK_TRUE(it != channelInfoMap.end());
    if (it != channelInfoMap.end())
    {
        CHECK_TRUE(it->second.eligible);
        CHECK_NEAR(it->second.sampleGain, 0.25f, 1.0e-6f);
        CHECK_NEAR(it->second.volume, 0.0f, 0.0f);
        CHECK_TRUE(it->second.routing.suppressNative());
        CHECK_TRUE(it->second.routing.playing);
    }
    LeaveCriticalSection(&stateLock);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);
}

void test_repeated_unpause_emits_only_one_play()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.5f);
    setGameVolume(fixture.channel, 0.5f);
    MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, BASS_MIXER_CHAN_PAUSE);
    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE) != DWORD(-1));
    clearEvents();

    int mixerCallsBefore = g_mock.mixerFlagsCalls;
    for (int i = 0; i < 4; ++i)
        CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE) != DWORD(-1));

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
    CHECK_EQ(g_mock.mixerFlagsCalls - mixerCallsBefore, 4);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(fixture.channel);
    CHECK_TRUE(it != channelInfoMap.end());
    if (it != channelInfoMap.end())
        CHECK_TRUE(it->second.routing.playing);
    LeaveCriticalSection(&stateLock);
}

void test_pause_resume_mixer_remove_readd_preserve_state()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    setGameVolume(fixture.channel, 0.5f);
    MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, BASS_MIXER_CHAN_PAUSE);
    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE) != DWORD(-1));
    clearEvents();

    const DWORD generation = fixture.generation;
    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, BASS_MIXER_CHAN_PAUSE,
                                         BASS_MIXER_CHAN_PAUSE) != DWORD(-1));
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));

    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE) != DWORD(-1));
    CHECK_TRUE(MyBASS_Mixer_ChannelRemove(fixture.channel));
    CHECK_TRUE(MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, 0));

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 2);
    CHECK_EQ(countState(events, CHANNEL_STATE_PAUSED), 2);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    CHECK_EQ(channelGeneration(fixture.channel), generation);

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(fixture.channel);
    CHECK_TRUE(it != channelInfoMap.end());
    if (it != channelInfoMap.end())
    {
        CHECK_TRUE(it->second.routing.suppressNative());
        CHECK_TRUE(it->second.routing.playing);
        CHECK_FALSE(it->second.paused);
        CHECK_NEAR(it->second.sampleGain, 0.25f, 1.0e-6f);
    }
    LeaveCriticalSection(&stateLock);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);
}

void test_stream_free_and_channel_stop_failure_keep_tracking()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    clearEvents();

    const DWORD generation = channelGeneration(fixture.channel);
    g_mock.stopOk = false;
    CHECK_FALSE(MyBASS_ChannelStop(fixture.channel));
    CHECK_TRUE(channelPresent(fixture.channel));
    CHECK_EQ(channelGeneration(fixture.channel), generation);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));

    g_mock.streamFreeOk = false;
    CHECK_FALSE(MyBASS_StreamFree(fixture.channel));
    CHECK_TRUE(channelPresent(fixture.channel));
    CHECK_EQ(channelGeneration(fixture.channel), generation);

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countRemoved(events, generation), 0);
    CHECK_EQ(countState(events, CHANNEL_STATE_PAUSED), 0);
}

void test_untracked_stream_free_publishes_nothing()
{
    resetFixture();
    clearEvents();
    // A music/preview stream never passed through SampleGetChannel.
    const HCHANNEL untracked = 0x7701;
    CHECK_FALSE(channelPresent(untracked));
    CHECK_TRUE(MyBASS_StreamFree(untracked));
    CHECK_TRUE(eventsSnapshot().empty());

    // A tracked lifetime still reports exactly one REMOVED with its generation.
    Fixture fixture = createChannel(true, 0.25f);
    const DWORD generation = channelGeneration(fixture.channel);
    CHECK_TRUE(generation != 0);
    clearEvents();
    CHECK_TRUE(MyBASS_StreamFree(fixture.channel));
    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(events.size(), 1);
    CHECK_EQ(countRemoved(events, generation), 1);
    CHECK_FALSE(channelPresent(fixture.channel));
}

// Debug Log alone must not write per-hit lines; the trace marker opts in.
// The sink is a real AsyncTextLog on a pipe, so the test reads what reaches it.
void test_hook_per_hit_lines_require_trace_marker()
{
    HANDLE pipeRead = nullptr, pipeWrite = nullptr;
    CHECK_TRUE(CreatePipe(&pipeRead, &pipeWrite, nullptr, 1 << 20) != FALSE);
    if (!pipeRead || !pipeWrite)
        return;
    int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(pipeWrite), _O_BINARY);
    FILE *sink = descriptor >= 0 ? _fdopen(descriptor, "wb") : nullptr;
    auto *log = new asio::AsyncTextLog;
    CHECK_TRUE(sink && log->start(sink));
    if (!sink)
    {
        CloseHandle(pipeRead);
        return;
    }
    auto *previous = hookTextLog;
    hookTextLog = log;

    auto runHit = [](bool trace) {
        resetFixture();
        debugLogEnabled = 1;
        detailedTraceEnabled = trace;
        Fixture fixture = createChannel(true, 0.25f);
        startExternal(fixture, 0.5f);
        MyBASS_ChannelPause(fixture.channel);
        detailedTraceEnabled = false;
        debugLogEnabled = 0;
    };
    // The log is FIFO: once a sentinel written after the hit arrives, every
    // earlier line has been flushed. A contention drop retries the whole hit.
    unsigned sentinel = 0;
    auto capture = [&](bool trace) {
        std::string text;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            const unsigned long droppedBefore = log->dropped();
            runHit(trace);
            char marker[64];
            std::snprintf(marker, sizeof(marker), "trace-test-sentinel-%u\n", ++sentinel);
            log->write("%s", marker);
            text.clear();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (text.find(marker) == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                DWORD available = 0;
                if (!PeekNamedPipe(pipeRead, nullptr, 0, nullptr, &available, nullptr))
                    break;
                if (available == 0)
                {
                    Sleep(1);
                    continue;
                }
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!ReadFile(pipeRead, chunk.data(), available, &read, nullptr) || read == 0)
                    break;
                text.append(chunk.data(), read);
            }
            if (log->dropped() == droppedBefore && text.find(marker) != std::string::npos)
                break;
        }
        return text;
    };

    std::string quiet = capture(false);
    CHECK_TRUE(quiet.find("trace-test-sentinel-") != std::string::npos);
    CHECK_TRUE(quiet.find("\"event\":\"start\"") == std::string::npos);
    CHECK_TRUE(quiet.find("\"event\":\"context\"") == std::string::npos);
    CHECK_TRUE(quiet.find("BASS_SampleGetChannel") == std::string::npos);
    // Sample publication is not per hit and stays in the Debug Log output.
    CHECK_TRUE(quiet.find("publishSample success") != std::string::npos);

    std::string traced = capture(true);
    CHECK_TRUE(traced.find("trace-test-sentinel-") != std::string::npos);
    CHECK_TRUE(traced.find("\"event\":\"start\"") != std::string::npos);
    CHECK_TRUE(traced.find("\"event\":\"pause\"") != std::string::npos);
    CHECK_TRUE(traced.find("BASS_SampleGetChannel") != std::string::npos);

    hookTextLog = previous;
    CloseHandle(pipeRead);
    CHECK_TRUE(log->stop());
    fclose(sink);
}

void test_handle_reuse_advances_generation()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    const DWORD firstGeneration = channelGeneration(fixture.channel);
    clearEvents();

    CHECK_TRUE(MyBASS_ChannelStop(fixture.channel));
    const DWORD secondGeneration = channelGeneration(fixture.channel);
    CHECK_TRUE(secondGeneration != firstGeneration);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Undecided));

    std::vector<Event> stopEvents = eventsSnapshot();
    CHECK_EQ(countRemoved(stopEvents, firstGeneration), 1);
    CHECK_EQ(countCreated(stopEvents, secondGeneration), 1);

    clearEvents();
    HCHANNEL reused = MyBASS_SampleGetChannel(fixture.sample, 0);
    CHECK_EQ(reused, fixture.channel);
    const DWORD thirdGeneration = channelGeneration(fixture.channel);
    CHECK_TRUE(thirdGeneration != secondGeneration);

    std::vector<Event> reuseEvents = eventsSnapshot();
    CHECK_EQ(countCreated(reuseEvents, thirdGeneration), 1);

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(fixture.channel);
    CHECK_TRUE(it != channelInfoMap.end());
    if (it != channelInfoMap.end())
    {
        CHECK_FALSE(it->second.eligible);
        CHECK_NEAR(it->second.sampleGain, 0.0f, 0.0f);
        CHECK_TRUE(it->second.routing.route == asio::Route::Undecided);
        CHECK_FALSE(it->second.routing.playing);
    }
    LeaveCriticalSection(&stateLock);
}

void test_queue_full_start_falls_back_native_without_mute()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    setGameVolume(fixture.channel, 0.5f);
    clearEvents();
    clearVolumeWrites();

    MyPool->head = 0;
    MyPool->tail = EVENT_POOL_SIZE - 1;
    CHECK_FALSE(queueAvailable());
    CHECK_TRUE(MyBASS_ChannelPlay(fixture.channel, FALSE));

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));
    CHECK_FALSE(anyZeroVolume(g_mock.volumeWrites));
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);

    clearEvents();
    MyPool->head = 0;
    MyPool->tail = 0;
    CHECK_TRUE(MyBASS_ChannelPlay(fixture.channel, FALSE));
    events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));
}

void test_bass_start_failure_restores_temporary_mute()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    setGameVolume(fixture.channel, 0.5f);
    clearVolumeWrites();

    g_mock.playOk = false;
    CHECK_FALSE(MyBASS_ChannelPlay(fixture.channel, FALSE));
    CHECK_EQ(g_mock.volumeWrites.size(), 2u);
    CHECK_NEAR(g_mock.volumeWrites.front().value, 0.0f, 0.0f);
    CHECK_NEAR(g_mock.volumeWrites.back().value, 0.5f, 0.0f);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Undecided));

    resetFixture();
    fixture = createChannel(true, 0.25f);
    setGameVolume(fixture.channel, 0.5f);
    clearVolumeWrites();
    g_mock.mixerAddOk = false;
    CHECK_FALSE(MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, 0));
    CHECK_EQ(g_mock.volumeWrites.size(), 2u);
    CHECK_NEAR(g_mock.volumeWrites.front().value, 0.0f, 0.0f);
    CHECK_NEAR(g_mock.volumeWrites.back().value, 0.5f, 0.0f);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Undecided));
}

void test_monitor_raw_decode_active_does_not_start_playback()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    g_mock.active = 1; // A raw decode channel may report active without a play request.
    clearEvents();
    clearVolumeWrites();

    pollerRunning = true;
    std::thread poller([] { monitorChannels(nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    pollerRunning = false;
    poller.join();

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
    CHECK_EQ(g_mock.isActiveCalls, 0);
    CHECK_EQ(g_mock.playCalls, 0);
    CHECK_EQ(g_mock.volumeWrites.size(), 0u);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Undecided));

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(fixture.channel);
    if (it != channelInfoMap.end())
        CHECK_TRUE(it->second.paused);
    LeaveCriticalSection(&stateLock);
}

void test_heartbeat_expiry_fails_over_and_restores_volume()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    clearEvents();
    clearVolumeWrites();

    InterlockedExchange(&MyPool->heartbeat, LONG(GetTickCount() - 500));
    CHECK_FALSE(backendAlive());

    std::vector<MonitorSnapshot> pending;
    monitorChannelsOnce(pending);
    CHECK_EQ(asio::recoveryState(MyPool),RECOVERY_REQUESTED);
    CHECK_TRUE(routeIs(fixture.channel,asio::Route::External));
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel],0.0f,0.0f);
    // A heartbeat alone cannot release ownership. The backend must stop first.
    InterlockedExchange(&MyPool->heartbeat,LONG(GetTickCount()));
    monitorChannelsOnce(pending);
    CHECK_EQ(asio::recoveryState(MyPool),RECOVERY_REQUESTED);
    InterlockedIncrement(&MyPool->epoch);
    InterlockedExchange(&MyPool->recoveryState,RECOVERY_BACKEND_STOPPED);
    monitorChannelsOnce(pending);
    CHECK_EQ(asio::recoveryState(MyPool),RECOVERY_REPLAYING);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));
    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PAUSED), 0);
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);

    EnterCriticalSection(&stateLock);
    auto it = channelInfoMap.find(fixture.channel);
    CHECK_TRUE(it != channelInfoMap.end());
    if (it != channelInfoMap.end())
    {
        CHECK_FALSE(it->second.routing.suppressNative());
        CHECK_FALSE(it->second.routing.playing);
    }
    LeaveCriticalSection(&stateLock);
}

// This is a separate case because a start-time fallback does not cover a
// transition whose state event is lost. If the queue cannot carry PAUSED, the
// only safe route is native, with the native volume restored.
void test_queue_full_during_pause_must_fail_over()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    clearEvents();
    clearVolumeWrites();

    MyPool->head = 0;
    MyPool->tail = EVENT_POOL_SIZE - 1;
    CHECK_FALSE(queueAvailable());
    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, BASS_MIXER_CHAN_PAUSE,
                                         BASS_MIXER_CHAN_PAUSE) != DWORD(-1));

    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PAUSED), 0); // the queue was full
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);
}

// A route that was External before pause must not resume when its sample ACK is
// no longer valid. Regression for H1: every External resume revalidates.
void test_resume_without_sample_ack_must_not_start_external()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));

    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, BASS_MIXER_CHAN_PAUSE,
                                         BASS_MIXER_CHAN_PAUSE) != DWORD(-1));
    clearEvents();
    clearVolumeWrites();

    auto &ack = MyPool->sampleAcks[fixture.sample % SAMPLE_ACK_COUNT];
    InterlockedExchange(&ack.readyGeneration, 0);
    CHECK_FALSE(sampleReady(fixture.sample));

    CHECK_TRUE(MyBASS_Mixer_ChannelFlags(fixture.channel, 0, BASS_MIXER_CHAN_PAUSE) != DWORD(-1));
    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);
}

// A stale managed context (old generation) must not tag a reused handle. The
// matching generation is the only one allowed to change eligibility/gain.
void test_v2_context_rejects_stale_generation_after_handle_reuse()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    const DWORD firstGeneration = fixture.generation;
    CHECK_TRUE(MyBASS_ChannelStop(fixture.channel));
    const DWORD secondGeneration = channelGeneration(fixture.channel);
    CHECK_TRUE(secondGeneration != firstGeneration);
    clearEvents();

    AsioChannelContextV2(fixture.channel, firstGeneration, 0.9f, 1);
    ChannelSnapshot stale = channelSnapshot(fixture.channel);
    CHECK_TRUE(stale.present);
    CHECK_FALSE(stale.eligible);
    // Stop keeps the last gain but clears eligibility; the stale context must
    // not overwrite either value or emit an update.
    CHECK_NEAR(stale.sampleGain, 0.25f, 0.0f);
    CHECK_TRUE(eventsSnapshot().empty());

    AsioChannelContextV2(fixture.channel, secondGeneration, 0.9f, 1);
    ChannelSnapshot current = channelSnapshot(fixture.channel);
    CHECK_TRUE(current.eligible);
    CHECK_NEAR(current.sampleGain, 0.9f, 1.0e-6f);
    CHECK_NEAR(latestUpdatedVolume(eventsSnapshot()), 0.9f, 1.0e-6f);

    // MyBASS_SampleGetChannel reuses the same handle and advances generation.
    HCHANNEL reused = MyBASS_SampleGetChannel(fixture.sample, 0);
    CHECK_EQ(reused, fixture.channel);
    const DWORD thirdGeneration = channelGeneration(fixture.channel);
    CHECK_TRUE(thirdGeneration != secondGeneration);
    clearEvents();

    AsioChannelContextV2(reused, secondGeneration, 0.7f, 1);
    ChannelSnapshot staleReuse = channelSnapshot(reused);
    CHECK_FALSE(staleReuse.eligible);
    CHECK_NEAR(staleReuse.sampleGain, 0.0f, 0.0f);
    CHECK_TRUE(eventsSnapshot().empty());

    AsioChannelContextV2(reused, thirdGeneration, 0.7f, 1);
    ChannelSnapshot currentReuse = channelSnapshot(reused);
    CHECK_TRUE(currentReuse.eligible);
    CHECK_NEAR(currentReuse.sampleGain, 0.7f, 1.0e-6f);

    setGameVolume(reused, 0.5f);
    clearEvents();
    CHECK_TRUE(MyBASS_ChannelPlay(reused, FALSE));
    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 1);
    for (const Event &event : events)
    {
        if (event.type == EVENT_CHANNEL_STATE && event.data.channelState.state == CHANNEL_STATE_PLAYING)
        {
            CHECK_EQ(event.generation, thirdGeneration);
            CHECK_NEAR(event.data.channelState.volume, 0.7f, 1.0e-6f);
        }
    }
    CHECK_TRUE(routeIs(reused, asio::Route::External));
}

// Two sample handles separated by exactly 4096 share a slot index. Full-handle
// probing must keep both ACKs alive and must not report a false hit for a third.
void test_unrelated_queries_bypass_state_lock()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    clearEvents();
    HANDLE locked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread owner([&] {
        EnterCriticalSection(&stateLock);
        SetEvent(locked);
        WaitForSingleObject(release, 2000);
        LeaveCriticalSection(&stateLock);
    });
    WaitForSingleObject(locked, 2000);
    HANDLE complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread query([&] {
        MyBASS_Mixer_ChannelFlags(fixture.channel, 0, 0);
        MyBASS_ChannelFlags(fixture.channel, 0, 0);
        MyBASS_ChannelSetAttribute(fixture.channel, 999, 0.5f);
        SetEvent(complete);
    });
    DWORD completed = WaitForSingleObject(complete, 500);
    SetEvent(release);
    owner.join();
    query.join();
    CHECK_EQ(completed, WAIT_OBJECT_0);
    CHECK_EQ(MyPool->head, MyPool->tail);
    CHECK_FALSE(channelSnapshot(fixture.channel).suppressNative);
    CloseHandle(complete);
    CloseHandle(locked);
    CloseHandle(release);
}

void test_monitor_revalidates_handle_after_unlocked_query()
{
    for (bool reuse : {false, true})
    {
        resetFixture();
        Fixture fixture = createChannel(true, 0.25f);
        startExternal(fixture, 0.5f);
        clearEvents();
        bool unlocked = false;
        g_activeFault = [&](DWORD handle) {
            std::thread mutation([&] {
                if (TryEnterCriticalSection(&stateLock))
                {
                    unlocked = true;
                    channelInfoMap.erase(handle);
                    if (reuse)
                    {
                        ChannelInfo replacement;
                        replacement.generation = fixture.generation + 100;
                        replacement.routing.route = asio::Route::External;
                        replacement.routing.playing = true;
                        replacement.paused = false;
                        channelInfoMap[handle] = replacement;
                    }
                    LeaveCriticalSection(&stateLock);
                }
            });
            mutation.join();
            g_mock.active = 0;
        };
        std::vector<MonitorSnapshot> pending;
        monitorChannelsOnce(pending);
        CHECK_TRUE(unlocked);
        CHECK_EQ(MyPool->head, MyPool->tail);
        if (reuse) CHECK_TRUE(channelInfoMap.at(fixture.channel).routing.playing);
        else CHECK_TRUE(channelInfoMap.count(fixture.channel) == 0);
    }
}

void test_monitor_ignores_inactive_result_after_same_generation_resume()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    clearEvents();
    DWORD generation = channelGeneration(fixture.channel);
    auto revision = channelInfoMap.at(fixture.channel).playbackRevision;
    bool resumed = false;
    g_activeFault = [&](DWORD handle) {
        MyBASS_Mixer_ChannelFlags(handle, BASS_MIXER_CHAN_PAUSE, BASS_MIXER_CHAN_PAUSE);
        MyBASS_Mixer_ChannelFlags(handle, 0, BASS_MIXER_CHAN_PAUSE);
        resumed = true;
        // Model an inactive observation already sampled before the resume.
        g_mock.active = 0;
    };
    std::vector<MonitorSnapshot> pending;
    monitorChannelsOnce(pending);
    auto events = eventsSnapshot();
    CHECK_TRUE(resumed);
    CHECK_EQ(channelGeneration(fixture.channel), generation);
    CHECK_TRUE(channelInfoMap.at(fixture.channel).playbackRevision != revision);
    CHECK_TRUE(channelInfoMap.at(fixture.channel).routing.playing);
    CHECK_FALSE(channelInfoMap.at(fixture.channel).paused);
    CHECK_TRUE(channelSnapshot(fixture.channel).suppressNative);
    CHECK_EQ(countState(events, CHANNEL_STATE_PAUSED), 1);
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 1);
}

void test_loop_failure_restores_only_matching_generation()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    g_mock.channelHandle += 1;
    Fixture unaffected = createChannel(true, 0.5f);
    startExternal(unaffected, 0.75f);
    CHECK_TRUE(publishChannelFailure(MyPool, fixture.channel, fixture.generation - 1));
    drainChannelFailures();
    CHECK_TRUE(channelSnapshot(fixture.channel).suppressNative);
    CHECK_TRUE(publishChannelFailure(MyPool, fixture.channel, fixture.generation));
    drainChannelFailures();
    CHECK_FALSE(channelSnapshot(fixture.channel).suppressNative);
    CHECK_NEAR(g_mock.nativeVolume.at(fixture.channel), 0.5f, 0.0f);
    CHECK_TRUE(channelSnapshot(unaffected.channel).suppressNative);
    CHECK_NEAR(g_mock.nativeVolume.at(unaffected.channel), 0.0f, 0.0f);
    CHECK_EQ(MyPool->backendReady, 1L);
    MyPool->failureHead = 0;
    MyPool->failureTail = CHANNEL_FAILURE_COUNT - 1;
    CHECK_FALSE(publishChannelFailure(MyPool, fixture.channel, fixture.generation));
    CHECK_EQ(MyPool->backendReady, 0L);
}

void test_shared_debug_matches_keyed_settings()
{
    resetFixture();
    const std::string inputs[] = {
        "[Config]\nDebug Log=0\nOutput Volume=50\nSample Rate=48000\nASIO Driver ID=0\nBuffer Size=16\n",
        "; 16 samples @ 48 kHz\n[Config]\nBuffer Size=16\nASIO Driver ID=0\nSample Rate=48000\nOutput Volume=50\nDebug Log=0\n",
        "; 777 unrelated\n[Config]\nDebug Log=1\nSample Rate=48000\nASIO Driver ID=0\nOutput Volume=50\nBuffer Size=16\n"
    };
    for (const auto &input : inputs) {
        asio::Settings parsed;
        std::string error;
        CHECK_TRUE(asio::parseSettings(input, parsed, error));
        MyPool->startupDebug = parsed.debug;
        MyPool->diagnosticsEnabled = parsed.debug;
        readSessionDiagnostics();
        CHECK_EQ(debugLogEnabled, parsed.debug);
        CHECK_EQ(traceEnabled.load(), parsed.debug != 0);
    }
    debugLogEnabled = 0;
    traceEnabled = false;
}

void test_wake_and_diagnostic_timestamps()
{
    resetFixture();
    asio::WakeEvent owner;
    CHECK_TRUE(owner.create(GetCurrentProcessId()));
    HANDLE wake = owner.get();
    wchar_t name[80];
    asio::wakeEventName(name, GetCurrentProcessId());
    hookWake = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    CHECK_TRUE(hookWake != nullptr);
    Event event = {};
    event.type = EVENT_CHANNEL_STATE;
    event.data.channelState.state = CHANNEL_STATE_PLAYING;
    CHECK_TRUE(pushEvent(event));
    CHECK_EQ(WaitForSingleObject(wake, 0), WAIT_OBJECT_0);
    CHECK_EQ(WaitForSingleObject(wake, 0), WAIT_TIMEOUT);
    CHECK_NEAR(float(MyPool->events[0].data.channelState.time), 0.0f, 0.0f);
    MyPool->diagnosticsEnabled = 1;
    double before = asio::qpcMilliseconds();
    CHECK_TRUE(pushEvent(event));
    CHECK_TRUE(MyPool->events[1].data.channelState.time >= before);
    CHECK_TRUE(MyPool->events[1].data.channelState.time <= asio::qpcMilliseconds());
    for (int i = 0; i < 20; ++i) CHECK_TRUE(pushEvent(event));
    CHECK_EQ(MyPool->tail, 22L);
    CHECK_EQ(WaitForSingleObject(wake, 0), WAIT_OBJECT_0);
    CloseHandle(hookWake);
    hookWake = nullptr;
    CHECK_TRUE(pushEvent(event));
}

void test_sample_ack_handle_collision_does_not_false_hit()
{
    resetFixture();
    const HSAMPLE first = 0x1101;
    const HSAMPLE second = 0x2101;
    CHECK_EQ(first % SAMPLE_ACK_COUNT, second % SAMPLE_ACK_COUNT);

    g_mock.sampleHandle = first;
    CHECK_EQ(MyBASS_SampleLoad(FALSE, nullptr, 0, 0, 0, 0), first);
    g_mock.sampleHandle = second;
    CHECK_EQ(MyBASS_SampleLoad(FALSE, nullptr, 0, 0, 0, 0), second);

    auto *firstAck = findSampleAck(first);
    auto *secondAck = findSampleAck(second);
    CHECK_TRUE(firstAck != nullptr);
    CHECK_TRUE(secondAck != nullptr);
    CHECK_TRUE(firstAck != secondAck);
    CHECK_EQ(DWORD(firstAck->sample), first);
    CHECK_EQ(DWORD(secondAck->sample), second);
    CHECK_TRUE(findSampleAck(0x3101) == nullptr);

    CHECK_FALSE(sampleReady(first));
    CHECK_FALSE(sampleReady(second));
    InterlockedExchange(&firstAck->readyGeneration, firstAck->requestedGeneration);
    CHECK_TRUE(sampleReady(first));
    CHECK_FALSE(sampleReady(second));
    CHECK_EQ(DWORD(secondAck->sample), second);
    InterlockedExchange(&secondAck->readyGeneration, secondAck->requestedGeneration);
    CHECK_TRUE(sampleReady(second));
    CHECK_EQ(DWORD(firstAck->sample), first);
    CHECK_EQ(DWORD(firstAck->readyGeneration), DWORD(firstAck->requestedGeneration));
    CHECK_EQ(sampleInfoMap.at(first).ackIndex, int(firstAck - MyPool->sampleAcks));
    CHECK_EQ(sampleInfoMap.at(second).ackIndex, int(secondAck - MyPool->sampleAcks));
    // Removing the first colliding sample must not break the cached second slot.
    CHECK_TRUE(MyBASS_SampleFree(first));
    CHECK_TRUE(sampleReady(second));
    InterlockedExchange(&secondAck->requestedGeneration, secondAck->requestedGeneration + 1);
    CHECK_FALSE(sampleReady(second));
    InterlockedExchange(&secondAck->requestedGeneration, secondAck->requestedGeneration - 1);
    CHECK_TRUE(sampleReady(second));
    InterlockedExchange(&secondAck->sample, LONG(first));
    CHECK_FALSE(sampleReady(second));
    InterlockedExchange(&secondAck->sample, LONG(second));
    CHECK_TRUE(sampleReady(second));

    // Capacity is explicit: with every slot occupied by a different handle,
    // allocation must fail closed instead of overwriting an existing ACK.
    for (int i = 0; i < SAMPLE_ACK_COUNT; ++i)
    {
        auto &ack = MyPool->sampleAcks[i];
        InterlockedExchange(&ack.sample, LONG(i + 1));
        InterlockedExchange(&ack.requestedGeneration, 1);
        InterlockedExchange(&ack.readyGeneration, 1);
    }
    CHECK_TRUE(findSampleAck(SAMPLE_ACK_COUNT + 1, true) == nullptr);
    CHECK_TRUE(findSampleAck(1) != nullptr);
}

// A failed native volume restore is not a successful failover. It must stay
// pending and be retried by the monitor once the BASS API recovers.
void test_restore_failure_sets_pending_and_monitor_retries()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    startExternal(fixture, 0.5f);
    clearEvents();
    clearVolumeWrites();

    InterlockedExchange(&MyPool->heartbeat, LONG(GetTickCount() - 500));
    asio::requestRecovery(MyPool);
    // Backend acknowledged stopping the old voices; native restore now runs.
    InterlockedIncrement(&MyPool->epoch);
    InterlockedExchange(&MyPool->recoveryState,RECOVERY_BACKEND_STOPPED);
    g_mock.setAttributeOk = false;

    pollerRunning = true;
    std::thread poller([] { monitorChannels(nullptr); });
    CHECK_TRUE(waitFor([&] { return channelSnapshot(fixture.channel).restorePending; }));
    pollerRunning = false;
    poller.join();

    ChannelSnapshot failed = channelSnapshot(fixture.channel);
    CHECK_TRUE(failed.restorePending);
    CHECK_FALSE(failed.suppressNative);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));
    CHECK_EQ(MyPool->backendReady, 0L);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);

    g_mock.setAttributeOk = true;
    pollerRunning = true;
    std::thread retry([] { monitorChannels(nullptr); });
    CHECK_TRUE(waitFor([&] {
        EnterCriticalSection(&stateLock);
        ChannelSnapshot current = channelSnapshot(fixture.channel);
        const auto native = g_mock.nativeVolume.find(fixture.channel);
        bool restored = !current.restorePending && native != g_mock.nativeVolume.end() && native->second == 0.5f;
        LeaveCriticalSection(&stateLock);
        return restored;
    }));
    pollerRunning = false;
    retry.join();

    ChannelSnapshot restored = channelSnapshot(fixture.channel);
    CHECK_FALSE(restored.restorePending);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);
}

// Queue overflow is not only a dropped event: the backend must be marked
// unhealthy so the native monitor fails over and the FMOD side pauses.
void test_queue_overflow_marks_backend_unready()
{
    resetFixture();
    MyPool->head = 0;
    MyPool->tail = EVENT_POOL_SIZE - 1;
    CHECK_EQ(MyPool->backendReady, 1L);

    Event event;
    std::memset(&event, 0, sizeof(event));
    event.type = EVENT_CHANNEL_STATE;
    CHECK_FALSE(pushEvent(event));
    CHECK_EQ(MyPool->backendReady, 0L);
    CHECK_FALSE(backendAlive());
    CHECK_EQ(MyPool->head, 0L);
    CHECK_EQ(MyPool->tail, EVENT_POOL_SIZE - 1L);

    MyPool->head = 0;
    MyPool->tail = 0;
    InterlockedExchange(&MyPool->backendReady, 1);
    CHECK_TRUE(pushEvent(event));
    CHECK_EQ(MyPool->tail, 1L);
}

// PLAY carries the full audible snapshot, so a lost CREATED/UPDATED cannot
// leave the FMOD channel at default gain/pan/frequency.
void test_play_event_snapshot_keeps_gain_pan_frequency()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    setGameVolume(fixture.channel, 0.5f);
    CHECK_TRUE(MyBASS_ChannelSetAttribute(fixture.channel, BASS_ATTRIB_PAN, -0.3f));
    CHECK_TRUE(MyBASS_ChannelSetAttribute(fixture.channel, BASS_ATTRIB_FREQ, 96000.0f));
    CHECK_TRUE(MyBASS_ChannelFlags(fixture.channel, BASS_SAMPLE_LOOP, BASS_SAMPLE_LOOP) != DWORD(-1));
    clearEvents();
    clearVolumeWrites();

    CHECK_TRUE(MyBASS_ChannelPlay(fixture.channel, FALSE));
    std::vector<Event> events = eventsSnapshot();
    CHECK_EQ(countCreated(events, fixture.generation), 0);
    CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 1);

    const Event *playing = nullptr;
    for (const Event &event : events)
        if (event.type == EVENT_CHANNEL_STATE && event.data.channelState.state == CHANNEL_STATE_PLAYING)
            playing = &event;
    CHECK_TRUE(playing != nullptr);
    if (playing)
    {
        CHECK_EQ(playing->generation, fixture.generation);
        CHECK_EQ(playing->data.channelState.sample, fixture.sample);
        CHECK_NEAR(playing->data.channelState.volume, 0.25f, 1.0e-6f);
        CHECK_NEAR(playing->data.channelState.pan, -0.3f, 1.0e-6f);
        CHECK_NEAR(playing->data.channelState.frequency, 96000.0f, 1.0e-6f);
        CHECK_EQ(playing->data.channelState.looping, 1L);
    }
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);
}

// H3: route decision, the real BASS call and the recorded game volume are
// serialized by stateLock. Another thread must be unable to enter the lock
// while the original ChannelSetAttribute is executing.
void test_set_attribute_holds_state_lock_across_original_call()
{
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);

    g_probeLockResult = -1;
    g_probeLockDuringSetAttribute = true;
    CHECK_TRUE(MyBASS_ChannelSetAttribute(fixture.channel, BASS_ATTRIB_VOL, 0.5f));
    CHECK_EQ(g_probeLockResult.load(), 0);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.5f, 0.0f);
    CHECK_NEAR(channelSnapshot(fixture.channel).volume, 0.5f, 0.0f);

    startExternal(fixture, 0.5f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);

    g_probeLockResult = -1;
    g_probeLockDuringSetAttribute = true;
    CHECK_TRUE(MyBASS_ChannelSetAttribute(fixture.channel, BASS_ATTRIB_VOL, 0.75f));
    CHECK_EQ(g_probeLockResult.load(), 0);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);
    ChannelSnapshot snapshot = channelSnapshot(fixture.channel);
    CHECK_TRUE(snapshot.suppressNative);
    CHECK_NEAR(snapshot.volume, 0.75f, 0.0f);
    CHECK_NEAR(snapshot.sampleGain, 0.25f, 1.0e-6f);
}

void test_play_query_revalidates_generation_and_revision()
{
    for(int mode=0;mode<4;++mode) {
        resetFixture();
        Fixture fixture=createChannel(true,0.25f);
        startExternal(fixture,0.5f);
        const DWORD original=channelGeneration(fixture.channel);
        const auto revision=channelInfoMap.at(fixture.channel).playbackRevision;
        clearEvents();
        bool unlocked=false;
        g_mock.active=1; // the externally mirrored native timeline is playing
        int callsBefore=g_mock.isActiveCalls;
        g_activeFault=[&](DWORD handle) {
            // Another real thread can take stateLock during the native query.
            std::thread mutation([&] {
                if(!TryEnterCriticalSection(&stateLock)) return;
                unlocked=true;
                auto &info=channelInfoMap.at(handle);
                if(mode==1) ++info.playbackRevision; // same generation resumed
                if(mode==2) {info.generation=DWORD(InterlockedIncrement(&nextGeneration));++info.playbackRevision;}
                if(mode==3) channelInfoMap.erase(handle);
                LeaveCriticalSection(&stateLock);
            });
            mutation.join();
            if(mode==1 || mode==2) {
                // First observation is stale 'stopped'. The serialized retry
                // observes current 'playing', avoiding a false new generation.
                g_mock.active=0;
                g_activeFault=[&](DWORD){g_mock.active=1;};
            }
        };
        CHECK_TRUE(MyBASS_ChannelPlay(fixture.channel,FALSE));
        CHECK_TRUE(unlocked);
        CHECK_EQ(g_mock.isActiveCalls-callsBefore,(mode==1||mode==2)?2:1);
        if(mode==3) {
            CHECK_FALSE(channelPresent(fixture.channel));
            CHECK_TRUE(eventsSnapshot().empty());
        } else {
            CHECK_EQ(channelGeneration(fixture.channel),mode==2?original+1:original);
            CHECK_TRUE(channelInfoMap.at(fixture.channel).playbackRevision>revision);
            auto events=eventsSnapshot();
            CHECK_EQ(countRemoved(events,original),0);
            CHECK_TRUE(routeIs(fixture.channel,asio::Route::External));
        }
    }
}

struct TestCase
{
    const char *name;
    void (*function)();
};

} // namespace

int main()
{
    InitializeCriticalSection(&queueLock);
    InitializeCriticalSection(&stateLock);
    InitializeCriticalSection(&logLock);
    locksInitialised = true;
    logLockInitialised = true;
    debugLogEnabled = 0;

    g_pool = new sharepool;
    MyPool = g_pool;
    installMocks();

    const TestCase tests[] = {
        {"untagged_ui_no_external_play_and_no_mute", test_untagged_ui_no_external_play_and_no_mute},
        {"tagged_zero_game_volume_still_emits_external_play_native_zero",
         test_tagged_zero_game_volume_still_emits_external_play_native_zero},
        {"repeated_unpause_emits_only_one_play", test_repeated_unpause_emits_only_one_play},
        {"pause_resume_mixer_remove_readd_preserve_state",
         test_pause_resume_mixer_remove_readd_preserve_state},
        {"stream_free_and_channel_stop_failure_keep_tracking",
         test_stream_free_and_channel_stop_failure_keep_tracking},
        {"untracked_stream_free_publishes_nothing", test_untracked_stream_free_publishes_nothing},
        {"hook_per_hit_lines_require_trace_marker", test_hook_per_hit_lines_require_trace_marker},
        {"handle_reuse_advances_generation", test_handle_reuse_advances_generation},
        {"queue_full_start_falls_back_native_without_mute",
         test_queue_full_start_falls_back_native_without_mute},
        {"bass_start_failure_restores_temporary_mute", test_bass_start_failure_restores_temporary_mute},
        {"monitor_raw_decode_active_does_not_start_playback",
         test_monitor_raw_decode_active_does_not_start_playback},
        {"heartbeat_expiry_fails_over_and_restores_volume",
         test_heartbeat_expiry_fails_over_and_restores_volume},
        {"queue_full_during_pause_must_fail_over", test_queue_full_during_pause_must_fail_over},
        {"resume_without_sample_ack_must_not_start_external",
         test_resume_without_sample_ack_must_not_start_external},
        {"v2_context_rejects_stale_generation_after_handle_reuse",
         test_v2_context_rejects_stale_generation_after_handle_reuse},
        {"unrelated_queries_bypass_state_lock", test_unrelated_queries_bypass_state_lock},
        {"play_query_revalidates_generation_and_revision", test_play_query_revalidates_generation_and_revision},
        {"monitor_revalidates_handle_after_unlocked_query", test_monitor_revalidates_handle_after_unlocked_query},
        {"monitor_ignores_inactive_result_after_same_generation_resume", test_monitor_ignores_inactive_result_after_same_generation_resume},
        {"loop_failure_restores_only_matching_generation", test_loop_failure_restores_only_matching_generation},
        {"shared_debug_matches_keyed_settings", test_shared_debug_matches_keyed_settings},
        {"wake_and_diagnostic_timestamps", test_wake_and_diagnostic_timestamps},
        {"sample_ack_handle_collision_does_not_false_hit",
         test_sample_ack_handle_collision_does_not_false_hit},
        {"restore_failure_sets_pending_and_monitor_retries",
         test_restore_failure_sets_pending_and_monitor_retries},
        {"queue_overflow_marks_backend_unready", test_queue_overflow_marks_backend_unready},
        {"play_event_snapshot_keeps_gain_pan_frequency",
         test_play_event_snapshot_keeps_gain_pan_frequency},
        {"set_attribute_holds_state_lock_across_original_call",
         test_set_attribute_holds_state_lock_across_original_call},
    };

    std::printf("native hook mock tests: %zu cases\n", sizeof(tests) / sizeof(tests[0]));
    std::printf("scope: mock BASS originals + heap sharepool; initDLL/MinHook/game/audio not run\n");
    for (const TestCase &test : tests)
    {
        const int failuresBefore = g_failures;
        std::printf("[ RUN      ] %s\n", test.name);
        try
        {
            test.function();
        }
        catch (const std::exception &exception)
        {
            fail(__FILE__, __LINE__, std::string("uncaught exception: ") + exception.what());
        }
        catch (...)
        {
            fail(__FILE__, __LINE__, "uncaught non-standard exception");
        }

        if (g_failures == failuresBefore)
            std::printf("[       OK ] %s\n", test.name);
        else
            std::printf("[  FAILED  ] %s\n", test.name);
    }

    std::printf("\nchecks: %d, failures: %d\n", g_checks, g_failures);
    if (g_failures == 0)
    {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
