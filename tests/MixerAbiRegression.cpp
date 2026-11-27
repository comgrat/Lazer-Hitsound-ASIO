// Independent BASSmix ABI regression for the production MyBASS_* detours.
//
// The unmodified NativeHookMock.cpp fixture is included after renaming its
// main(), so this TU uses the fixture's mock BASS originals and heap sharepool
// while calling the real production detour bodies. cl must be given
// /I <target-repository>\tests so the fixture and its ../dllmain.cpp come from
// the repository being tested.
//
// All mixer flag inputs below are hard-coded official values. BASS_MIXER_CHAN_*
// from the production sharepool.h is deliberately NOT used as an input, so a
// wrong production constant cannot make the test agree with itself. The
// production constant is checked as a separate runtime assertion (never a
// static_assert, so an unfixed tree still compiles and fails at run time).
//
// Scope: process-local mock BASS + heap sharepool only. No bass.dll/bassmix.dll,
// no output/ASIO device, no game, no account or audio data. This is not an
// acoustic acceptance test.

#define main NativeHookMockProgramMain
#include <NativeHookMock.cpp>
#undef main

namespace
{
constexpr DWORD kOfficialBuffer = 0x2000;
constexpr DWORD kOfficialNoRampIn = 0x800000;
constexpr DWORD kOfficialPause = 0x20000;
constexpr DWORD kOfficialLoop = 0x4;
constexpr DWORD kAddFlags = kOfficialBuffer | kOfficialNoRampIn | kOfficialPause;

bool allVolumeWritesEqual(const std::vector<VolumeWrite> &writes, float expected)
{
    return std::all_of(writes.begin(), writes.end(), [expected](const VolumeWrite &write) {
        return write.value == expected;
    });
}

DWORD queryMixerFlags(HCHANNEL channel)
{
    return MyBASS_Mixer_ChannelFlags(channel, 0, 0);
}

DWORD unpauseMixer(HCHANNEL channel)
{
    return MyBASS_Mixer_ChannelFlags(channel, 0, kOfficialPause);
}

DWORD pauseMixer(HCHANNEL channel)
{
    return MyBASS_Mixer_ChannelFlags(channel, kOfficialPause, kOfficialPause);
}

void checkMixerFlagsUpdated(HCHANNEL channel, DWORD returned, DWORD expectedPause)
{
    DWORD query = queryMixerFlags(channel);
    CHECK_TRUE(returned != DWORD(-1));
    CHECK_EQ(returned, query);
    CHECK_EQ(returned & kOfficialPause, expectedPause);
    CHECK_EQ(returned & kOfficialBuffer, kOfficialBuffer);
}

enum class SampleLoad { Ready, Pending, Failed };

void runVolumeScenario(float gameVolume, bool tagged, SampleLoad sample)
{
    const int failuresBefore = g_failures;
    const bool expectedExternal = tagged && sample != SampleLoad::Failed;
    const char *sampleState = sample == SampleLoad::Ready ? "ready" : sample == SampleLoad::Pending ? "pending" : "failed";
    std::printf("[ RUN      ] game_volume=%.2f tagged=%d sample=%s expected=%s\n",
                gameVolume, tagged ? 1 : 0, sampleState,
                expectedExternal ? "external/PLAY+mute" : "native/no PLAY+no mute");

    resetFixture();
    Fixture fixture = createChannel(tagged, 0.25f, sample == SampleLoad::Ready);
    CHECK_TRUE(fixture.channel != 0);
    if (fixture.channel == 0)
        return;
    if (sample == SampleLoad::Failed)
        InterlockedExchange(&findSampleAck(fixture.sample)->failedGeneration, LONG(fixture.sampleGeneration));

    setGameVolume(fixture.channel, gameVolume);
    clearEvents();
    clearVolumeWrites();

    CHECK_TRUE(MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, kAddFlags));
    CHECK_EQ(queryMixerFlags(fixture.channel), kAddFlags);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], gameVolume, 0.0f);
    std::vector<Event> beforeEvents = eventsSnapshot();
    CHECK_EQ(countState(beforeEvents, CHANNEL_STATE_PLAYING), 0);
    clearEvents();
    clearVolumeWrites();

    DWORD returned = unpauseMixer(fixture.channel);
    DWORD query = queryMixerFlags(fixture.channel);
    CHECK_TRUE(returned != DWORD(-1));
    CHECK_EQ(returned, query);
    CHECK_EQ(returned & kOfficialPause, 0);
    CHECK_EQ(returned & kOfficialBuffer, kOfficialBuffer);

    std::vector<Event> events = eventsSnapshot();
    ChannelSnapshot snapshot = channelSnapshot(fixture.channel);
    if (expectedExternal)
    {
        CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 1);
        CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
        CHECK_TRUE(snapshot.suppressNative);
        CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);
        CHECK_TRUE(!g_mock.volumeWrites.empty());
        CHECK_TRUE(anyZeroVolume(g_mock.volumeWrites));
    }
    else
    {
        CHECK_EQ(countState(events, CHANNEL_STATE_PLAYING), 0);
        CHECK_TRUE(routeIs(fixture.channel, asio::Route::Native));
        CHECK_FALSE(snapshot.suppressNative);
        CHECK_NEAR(g_mock.nativeVolume[fixture.channel], gameVolume, 0.0f);
        CHECK_TRUE(allVolumeWritesEqual(g_mock.volumeWrites, gameVolume));
    }

    if (g_failures == failuresBefore)
        std::printf("[       OK ] game_volume=%.2f tagged=%d sample=%s\n",
                    gameVolume, tagged ? 1 : 0, sampleState);
}

void runPauseResumeScenario()
{
    const int failuresBefore = g_failures;
    std::printf("[ RUN      ] tagged pause/resume uses official PAUSE mask 0x%08lX\n",
                static_cast<unsigned long>(kOfficialPause));
    resetFixture();
    Fixture fixture = createChannel(true, 0.25f);
    CHECK_TRUE(fixture.channel != 0);
    if (fixture.channel == 0)
        return;

    setGameVolume(fixture.channel, 0.5f);
    CHECK_TRUE(MyBASS_Mixer_StreamAddChannel(0x9001, fixture.channel, kAddFlags));
    clearEvents();
    clearVolumeWrites();

    DWORD firstPlay = unpauseMixer(fixture.channel);
    checkMixerFlagsUpdated(fixture.channel, firstPlay, 0);
    std::vector<Event> playEvents = eventsSnapshot();
    CHECK_EQ(countState(playEvents, CHANNEL_STATE_PLAYING), 1);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);

    clearEvents();
    clearVolumeWrites();
    DWORD paused = pauseMixer(fixture.channel);
    checkMixerFlagsUpdated(fixture.channel, paused, kOfficialPause);
    std::vector<Event> pauseEvents = eventsSnapshot();
    CHECK_EQ(countState(pauseEvents, CHANNEL_STATE_PAUSED), 1);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));

    clearEvents();
    clearVolumeWrites();
    DWORD resumed = unpauseMixer(fixture.channel);
    checkMixerFlagsUpdated(fixture.channel, resumed, 0);
    std::vector<Event> resumeEvents = eventsSnapshot();
    CHECK_EQ(countState(resumeEvents, CHANNEL_STATE_PLAYING), 1);
    CHECK_TRUE(routeIs(fixture.channel, asio::Route::External));
    CHECK_TRUE(channelSnapshot(fixture.channel).suppressNative);
    CHECK_NEAR(g_mock.nativeVolume[fixture.channel], 0.0f, 0.0f);

    if (g_failures == failuresBefore)
        std::printf("[       OK ] tagged pause/resume\n");
}

void runLoopReturnScenario()
{
    const int failuresBefore = g_failures;
    std::printf("[ RUN      ] MyBASS_ChannelFlags loop return is updated flags\n");
    resetFixture();
    Fixture fixture = createChannel(false, 0.25f);
    CHECK_TRUE(fixture.channel != 0);
    if (fixture.channel == 0)
        return;

    DWORD returned = MyBASS_ChannelFlags(fixture.channel, kOfficialLoop, kOfficialLoop);
    DWORD query = MyBASS_ChannelFlags(fixture.channel, 0, 0);
    CHECK_TRUE(returned != DWORD(-1));
    CHECK_EQ(returned, query);
    CHECK_EQ(returned & kOfficialLoop, kOfficialLoop);
    if (g_failures == failuresBefore)
        std::printf("[       OK ] MyBASS_ChannelFlags loop return\n");
}
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

    std::printf("mixer ABI regression: unmodified NativeHookMock fixture + real MyBASS_* detours\n");
    std::printf("scope: mock BASS originals + heap sharepool; no DLL/device/game/account/audio\n");
    std::printf("inputs: BUFFER=0x%08lX NORAMPIN=0x%08lX PAUSE=0x%08lX mask=0x%08lX\n",
                static_cast<unsigned long>(kOfficialBuffer),
                static_cast<unsigned long>(kOfficialNoRampIn),
                static_cast<unsigned long>(kOfficialPause),
                static_cast<unsigned long>(kOfficialPause));

    // Runtime-only check of the production constant. Do not convert this to a
    // static_assert: an unfixed sharepool.h must still compile and fail live.
    CHECK_EQ(DWORD(BASS_MIXER_CHAN_PAUSE), kOfficialPause);

    const float volumes[] = {0.0f, 0.01f, 0.25f, 0.5f, 1.0f};
    for (float volume : volumes)
    {
        runVolumeScenario(volume, true, SampleLoad::Ready);
        runVolumeScenario(volume, false, SampleLoad::Ready);
    }
    runVolumeScenario(0.0f, true, SampleLoad::Pending);
    runVolumeScenario(0.5f, true, SampleLoad::Failed);
    runPauseResumeScenario();
    runLoopReturnScenario();

    std::printf("\nchecks: %d, failures: %d\n", g_checks, g_failures);
    if (g_failures == 0)
    {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
