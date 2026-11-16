#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <algorithm>
#include <cwchar>
#include <cmath>

namespace asio {
inline void wakeEventName(wchar_t (&name)[80], DWORD pid)
{
    swprintf_s(name, L"Local\\OsuAsioV2Wake_%lu", static_cast<unsigned long>(pid));
}

class TimerResolution {
    bool active = false;
public:
    TimerResolution() : active(timeBeginPeriod(1) == TIMERR_NOERROR) {}
    ~TimerResolution() { if (active) timeEndPeriod(1); }
    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;
};

class WakeEvent {
    HANDLE handle = nullptr;
public:
    ~WakeEvent() { if (handle) CloseHandle(handle); }
    bool create(DWORD pid)
    {
        if (handle) return false;
        wchar_t name[80];
        wakeEventName(name, pid);
        handle = CreateEventW(nullptr, FALSE, FALSE, name);
        return handle != nullptr;
    }
    HANDLE get() const { return handle; }
};

inline double qpcMilliseconds()
{
    static const double scale = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1000.0 / static_cast<double>(frequency.QuadPart);
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * scale;
}

struct QueueLatency {
    static constexpr size_t capacity = 4096;
    std::array<double, capacity> values{};
    size_t count = 0;
    size_t cursor = 0;
    void add(double milliseconds)
    {
        if (!std::isfinite(milliseconds) || milliseconds < 0) return;
        values[cursor] = milliseconds;
        cursor = (cursor + 1) % capacity;
        if (count < capacity) ++count;
    }
    struct Summary { size_t count; double p50, p95, maximum; };
    Summary summary() const
    {
        if (!count) return {0, 0, 0, 0};
        auto sorted = values;
        std::sort(sorted.begin(), sorted.begin() + count);
        return {count, sorted[(count - 1) / 2], sorted[((count * 95 + 99) / 100) - 1], sorted[count - 1]};
    }
};
}
