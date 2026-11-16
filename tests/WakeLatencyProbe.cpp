// Synthetic producer/consumer queue benchmark, no game, BASS or hardware.
// Separate process invocations keep timeBeginPeriod local to the new-path run.
#include "../event_wake.h"
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>
#include <memory>

int main(int argc, char** argv)
{
    if (argc != 2 || (strcmp(argv[1], "--old") && strcmp(argv[1], "--new"))) return 2;
    bool eventDriven = !strcmp(argv[1], "--new");
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return 1;
    std::atomic<bool> running{true};
    std::atomic<bool> ready{false};
    std::atomic<unsigned> produced{0}, consumed{0};
    std::atomic<double> queued{0};
    asio::QueueLatency latency;
    std::thread consumer([&] {
        std::unique_ptr<asio::TimerResolution> timer;
        if (eventDriven) timer = std::make_unique<asio::TimerResolution>();
        ready = true;
        while (running) {
            unsigned sequence = produced.load(std::memory_order_acquire);
            if (sequence != consumed.load(std::memory_order_relaxed)) {
                latency.add(asio::qpcMilliseconds() - queued.load(std::memory_order_acquire));
                consumed.store(sequence, std::memory_order_release);
            } else if (eventDriven) WaitForSingleObject(wake, 5);
            else Sleep(1);
        }
    });
    while (!ready) SwitchToThread();
    const unsigned samples = 256;
    for (unsigned i = 1; i <= samples; ++i) {
        // Producer busy-waits outside the consumer, so it does not request a
        // timer resolution which could change the old-path observation.
        double target = asio::qpcMilliseconds() + 0.2 + (i % 7) * 0.07;
        while (asio::qpcMilliseconds() < target) SwitchToThread();
        queued.store(asio::qpcMilliseconds(), std::memory_order_release);
        produced.store(i, std::memory_order_release);
        if (eventDriven) SetEvent(wake);
        ULONGLONG deadline = GetTickCount64() + 2000;
        while (consumed.load(std::memory_order_acquire) != i) {
            if (GetTickCount64() > deadline) {
                running = false;
                SetEvent(wake);
                consumer.join();
                CloseHandle(wake);
                return 1;
            }
            SwitchToThread();
        }
    }
    running = false;
    SetEvent(wake);
    consumer.join();
    CloseHandle(wake);
    auto result = latency.summary();
    printf("WakeLatencyProbe mode=%s n=%zu p50_ms=%.6f p95_ms=%.6f max_ms=%.6f; synthetic Windows QPC queue, no acoustic latency claim\n",
        eventDriven ? "event+timer1ms" : "Sleep(1)-no-timer-request", result.count, result.p50, result.p95, result.maximum);
    return result.count == samples ? 0 : 1;
}
