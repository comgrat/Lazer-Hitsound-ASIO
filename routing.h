#pragma once
#include <cmath>

namespace asio
{
enum class Route { Undecided, Native, External };
struct RoutingState
{
    Route route = Route::Undecided;
    bool playing = false;

    bool start(bool tagged, bool backendAlive, bool sampleReady, bool queueAccepted)
    {
        if (route == Route::Undecided)
            route = tagged && backendAlive && sampleReady && queueAccepted ? Route::External : Route::Native;
        if (route != Route::External)
            return false;
        if (!tagged || !backendAlive || !sampleReady || !queueAccepted)
        {
            failover();
            return false;
        }
        if (playing)
            return false;
        playing = true;
        return true;
    }
    void pause() { playing = false; }
    void failover() { route = Route::Native; playing = false; }
    bool suppressNative() const { return route == Route::External; }
};

inline float outputGain(float sampleGain, int outputPercent)
{
    if (!std::isfinite(sampleGain) || sampleGain < 0 || sampleGain > 1 || outputPercent < 0 || outputPercent > 100)
        return 0;
    return sampleGain * (outputPercent / 100.0f);
}
}
