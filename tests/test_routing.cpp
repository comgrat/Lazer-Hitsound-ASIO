// Behavioral regression tests for the pure routing contract in routing.h.
//
// Scope: C++17 routing decisions and output gain arithmetic only.
// Not covered here: BASS/FMOD hooks, shared-memory transport, heartbeat,
// managed startup hook, device output, and any acoustic acceptance.
//
// The product requirement is that the ASIO path keeps chart gain when the
// game hitsound volume is 0, and that it is not attenuating by the user
// slider at non-zero values. outputGain therefore takes only the original
// sample gain and the independent ASIO volume; the game/user volume is not
// an input at all. The tests below exercise that contract directly.

#include "../routing.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
int g_checks = 0;
int g_failures = 0;

const char *routeName(asio::Route route)
{
    switch (route)
    {
    case asio::Route::Undecided: return "Undecided";
    case asio::Route::Native: return "Native";
    case asio::Route::External: return "External";
    }
    return "Unknown";
}

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

void checkRoute(const asio::RoutingState &state, asio::Route expected, const char *file, int line)
{
    ++g_checks;
    if (state.route != expected)
    {
        char message[160] = {};
        std::snprintf(message, sizeof(message), "route == %s (actual %s)", routeName(expected),
                      routeName(state.route));
        fail(file, line, message);
    }
}

#define CHECK_TRUE(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define CHECK_FALSE(expression) check(!(expression), "!(" #expression ")", __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, epsilon) \
    checkNear((actual), (expected), (epsilon), #actual, #expected, __FILE__, __LINE__)
#define CHECK_ROUTE(state, expected) checkRoute((state), (expected), __FILE__, __LINE__)

// The public contract is intentionally only (sampleGain, outputPercent).
// If a user/game aggregate volume is added to this signature, the test fails
// to compile instead of silently restoring the coupling this project removes.
static_assert(std::is_same_v<decltype(&asio::outputGain), float (*)(float, int)>,
              "outputGain must take only the original sample gain and ASIO output volume");

void test_default_state_is_undecided()
{
    asio::RoutingState state;
    CHECK_ROUTE(state, asio::Route::Undecided);
    CHECK_FALSE(state.playing);
    CHECK_FALSE(state.suppressNative());
}

void test_output_gain_scales_sample_gain()
{
    // This unit only accepts sample gain and ASIO volume. Real user-volume
    // changes belong to LiveGraphResearch, which changes the actual bindables.
    for (float sampleGain : {0.0f, 0.25f, 0.5f, 1.0f})
        CHECK_NEAR(asio::outputGain(sampleGain, 100), sampleGain, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(0.8f, 100), 0.8f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(0.8f, 50), 0.4f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(0.8f, 25), 0.2f, 1.0e-6f);
}

void test_output_gain_preserves_chart_gain()
{
    // Chart gain 0 stays silent by design. This is distinct from the user
    // volume 0 case above: the original sample gain itself is zero.
    CHECK_NEAR(asio::outputGain(0.0f, 0), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(0.0f, 50), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(0.0f, 100), 0.0f, 0.0f);

    CHECK_NEAR(asio::outputGain(0.25f, 100), 0.25f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(0.5f, 100), 0.5f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(1.0f, 100), 1.0f, 1.0e-6f);
}

void test_output_gain_applies_asio_volume_exactly_once()
{
    CHECK_NEAR(asio::outputGain(1.0f, 0), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(1.0f, 1), 0.01f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(1.0f, 25), 0.25f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(1.0f, 50), 0.5f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(1.0f, 100), 1.0f, 0.0f);

    // 0.5 * 50% is 0.25, never 0.125 (double application).
    CHECK_NEAR(asio::outputGain(0.5f, 50), 0.25f, 1.0e-6f);
    CHECK_NEAR(asio::outputGain(0.3f, 33), 0.099f, 1.0e-6f);
}

void test_output_gain_rejects_invalid_input()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float positiveInfinity = std::numeric_limits<float>::infinity();
    const float negativeInfinity = -std::numeric_limits<float>::infinity();

    CHECK_NEAR(asio::outputGain(nan, 100), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(positiveInfinity, 100), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(negativeInfinity, 100), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(-0.001f, 100), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(1.001f, 100), 0.0f, 0.0f);

    CHECK_NEAR(asio::outputGain(0.5f, -1), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(0.5f, 101), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(nan, -1), 0.0f, 0.0f);

    // Valid boundary values remain accepted.
    CHECK_NEAR(asio::outputGain(0.0f, 0), 0.0f, 0.0f);
    CHECK_NEAR(asio::outputGain(1.0f, 100), 1.0f, 0.0f);
}

void test_ui_untagged_channel_is_never_taken_over()
{
    // UI, music and storyboard channels are not tagged as GameplayMixer.
    // No combination of backend/sample/queue readiness may route them out.
    for (int mask = 0; mask < 8; ++mask)
    {
        const bool backendAlive = (mask & 1) != 0;
        const bool sampleReady = (mask & 2) != 0;
        const bool queueAccepted = (mask & 4) != 0;

        asio::RoutingState state;
        CHECK_FALSE(state.start(false, backendAlive, sampleReady, queueAccepted));
        CHECK_ROUTE(state, asio::Route::Native);
        CHECK_FALSE(state.playing);
        CHECK_FALSE(state.suppressNative());

        // A native channel never upgrades later, even after all prerequisites
        // become true or the channel is paused.
        state.pause();
        CHECK_FALSE(state.start(true, true, true, true));
        CHECK_ROUTE(state, asio::Route::Native);
    }
}

void test_unready_backend_sample_or_queue_falls_back_native()
{
    struct Case
    {
        bool backendAlive;
        bool sampleReady;
        bool queueAccepted;
        const char *name;
    };
    const Case cases[] = {
        {false, true, true, "backend unavailable"},
        {true, false, true, "sample not published"},
        {true, true, false, "queue precheck rejected"},
    };

    for (const Case &testCase : cases)
    {
        asio::RoutingState state;
        CHECK_FALSE(state.start(true, testCase.backendAlive, testCase.sampleReady, testCase.queueAccepted));
        CHECK_ROUTE(state, asio::Route::Native);
        CHECK_FALSE(state.playing);
        CHECK_FALSE(state.suppressNative());

        // The first decision is final: later readiness must not upgrade.
        CHECK_FALSE(state.start(true, true, true, true));
        CHECK_ROUTE(state, asio::Route::Native);
        if (state.route != asio::Route::Native)
            std::printf("    case: %s\n", testCase.name);
    }
}

void test_first_eligible_start_routes_external()
{
    asio::RoutingState state;
    CHECK_TRUE(state.start(true, true, true, true));
    CHECK_ROUTE(state, asio::Route::External);
    CHECK_TRUE(state.playing);
    CHECK_TRUE(state.suppressNative());
}

void test_repeated_start_emits_only_one_external_play()
{
    asio::RoutingState state;
    int emitted = 0;
    for (int i = 0; i < 1000; ++i)
    {
        if (state.start(true, true, true, true))
            ++emitted;
    }
    CHECK_TRUE(emitted == 1);
    CHECK_TRUE(state.playing);
    CHECK_ROUTE(state, asio::Route::External);
    CHECK_TRUE(state.suppressNative());
}

void test_pause_then_resume_on_same_external_route()
{
    asio::RoutingState state;
    CHECK_TRUE(state.start(true, true, true, true));

    state.pause();
    CHECK_FALSE(state.playing);
    CHECK_ROUTE(state, asio::Route::External);
    CHECK_TRUE(state.suppressNative());

    // Pausing twice is idempotent; a paused external route may resume.
    state.pause();
    CHECK_TRUE(state.start(true, true, true, true));
    CHECK_TRUE(state.playing);
    CHECK_TRUE(state.suppressNative());
    CHECK_FALSE(state.start(true, true, true, true));

    const int userVolumes[] = {0, 1, 25, 50, 100};
    for (int userVolume : userVolumes)
    {
        (void)userVolume;
        CHECK_NEAR(asio::outputGain(0.75f, 100), 0.75f, 1.0e-6f);
    }
}

void test_failover_never_upgrades_again()
{
    asio::RoutingState state;
    CHECK_TRUE(state.start(true, true, true, true));
    state.failover();

    CHECK_ROUTE(state, asio::Route::Native);
    CHECK_FALSE(state.playing);
    CHECK_FALSE(state.suppressNative());

    for (int i = 0; i < 16; ++i)
        CHECK_FALSE(state.start(true, true, true, true));

    state.pause();
    CHECK_FALSE(state.start(true, true, true, true));
    CHECK_ROUTE(state, asio::Route::Native);
}

void test_queue_commit_failure_fails_over_native()
{
    asio::RoutingState state;
    CHECK_TRUE(state.start(true, true, true, true));
    state.pause();

    // queueAccepted models the pre-submit capacity check. A failed submit is
    // represented by a subsequent start with queueAccepted=false and must
    // permanently downgrade this channel.
    CHECK_FALSE(state.start(true, true, true, false));
    CHECK_ROUTE(state, asio::Route::Native);
    CHECK_FALSE(state.playing);
    CHECK_FALSE(state.suppressNative());
    CHECK_FALSE(state.start(true, true, true, true));
}

void test_backend_loss_after_pause_fails_over_native()
{
    asio::RoutingState state;
    CHECK_TRUE(state.start(true, true, true, true));
    state.pause();

    CHECK_FALSE(state.start(true, false, true, true));
    CHECK_ROUTE(state, asio::Route::Native);
    CHECK_FALSE(state.playing);
    CHECK_FALSE(state.suppressNative());
    CHECK_FALSE(state.start(true, true, true, true));
}

void test_interleaved_channel_event_order()
{
    // Deterministic interleaving of the event sequences that production
    // serializes per channel. Each channel owns a separate RoutingState.
    asio::RoutingState channels[4];

    CHECK_TRUE(channels[0].start(true, true, true, true)); // ch0 external
    CHECK_FALSE(channels[1].start(false, true, true, true)); // ch1 UI -> native
    CHECK_FALSE(channels[2].start(true, true, true, false)); // ch2 queue full -> native
    CHECK_TRUE(channels[3].start(true, true, true, true)); // ch3 external

    channels[0].pause();
    CHECK_FALSE(channels[1].start(true, true, true, true)); // native stays native
    channels[2].failover();
    CHECK_FALSE(channels[3].start(true, true, true, true)); // duplicate start rejected
    CHECK_TRUE(channels[0].start(true, true, true, true)); // resume after pause

    channels[3].pause();
    CHECK_FALSE(channels[3].start(true, false, true, true)); // backend lost -> native
    CHECK_FALSE(channels[3].start(true, true, true, true)); // no upgrade

    channels[0].failover();
    CHECK_FALSE(channels[0].start(true, true, true, true)); // no upgrade after failover

    CHECK_ROUTE(channels[0], asio::Route::Native);
    CHECK_ROUTE(channels[1], asio::Route::Native);
    CHECK_ROUTE(channels[2], asio::Route::Native);
    CHECK_ROUTE(channels[3], asio::Route::Native);
    for (const asio::RoutingState &channel : channels)
        CHECK_FALSE(channel.playing);
    for (const asio::RoutingState &channel : channels)
        CHECK_FALSE(channel.suppressNative());
}

void test_serialized_concurrent_start_emits_once()
{
    // RoutingState has no internal synchronization. Production must serialize
    // events per channel; this test uses that same serialization to prove that
    // a concurrent burst still emits exactly one external start per play and
    // exactly one resume after pause. It does not claim RoutingState itself is
    // thread-safe.
    asio::RoutingState state;
    std::mutex stateMutex;
    constexpr int threadCount = 8;
    constexpr int attemptsPerThread = 200;

    auto runBurst = [&](std::atomic<int> &emitted, std::atomic<int> &attempts) {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (int i = 0; i < threadCount; ++i)
        {
            threads.emplace_back([&]() {
                for (int attempt = 0; attempt < attemptsPerThread; ++attempt)
                {
                    ++attempts;
                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (state.start(true, true, true, true))
                        ++emitted;
                }
            });
        }
        for (std::thread &thread : threads)
            thread.join();
    };

    std::atomic<int> attempts{0};
    std::atomic<int> emitted{0};
    runBurst(emitted, attempts);

    CHECK_TRUE(attempts == threadCount * attemptsPerThread);
    CHECK_TRUE(emitted == 1);
    CHECK_TRUE(state.playing);
    CHECK_ROUTE(state, asio::Route::External);
    CHECK_TRUE(state.suppressNative());

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state.pause();
    }

    std::atomic<int> resumeAttempts{0};
    std::atomic<int> resumed{0};
    runBurst(resumed, resumeAttempts);

    CHECK_TRUE(resumeAttempts == threadCount * attemptsPerThread);
    CHECK_TRUE(resumed == 1);
    CHECK_TRUE(state.playing);
    CHECK_TRUE(state.suppressNative());
}

struct TestCase
{
    const char *name;
    void (*function)();
};

} // namespace

int main()
{
    const TestCase tests[] = {
        {"default_state_is_undecided", test_default_state_is_undecided},
        {"output_gain_scales_sample_gain",
         test_output_gain_scales_sample_gain},
        {"output_gain_preserves_chart_gain", test_output_gain_preserves_chart_gain},
        {"output_gain_applies_asio_volume_exactly_once",
         test_output_gain_applies_asio_volume_exactly_once},
        {"output_gain_rejects_invalid_input", test_output_gain_rejects_invalid_input},
        {"ui_untagged_channel_is_never_taken_over", test_ui_untagged_channel_is_never_taken_over},
        {"unready_backend_sample_or_queue_falls_back_native",
         test_unready_backend_sample_or_queue_falls_back_native},
        {"first_eligible_start_routes_external", test_first_eligible_start_routes_external},
        {"repeated_start_emits_only_one_external_play",
         test_repeated_start_emits_only_one_external_play},
        {"pause_then_resume_on_same_external_route", test_pause_then_resume_on_same_external_route},
        {"failover_never_upgrades_again", test_failover_never_upgrades_again},
        {"queue_commit_failure_fails_over_native", test_queue_commit_failure_fails_over_native},
        {"backend_loss_after_pause_fails_over_native", test_backend_loss_after_pause_fails_over_native},
        {"interleaved_channel_event_order", test_interleaved_channel_event_order},
        {"serialized_concurrent_start_emits_once", test_serialized_concurrent_start_emits_once},
    };

    std::printf("routing contract tests: %zu cases\n", sizeof(tests) / sizeof(tests[0]));
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
