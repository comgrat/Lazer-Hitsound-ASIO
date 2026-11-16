#pragma once
#include <windows.h>
#include "sharepool.h"

// Single backend writer, single native-monitor reader. A full failure ring is
// state loss and disables the backend; it never silently loses ownership loss.
inline bool publishChannelFailure(sharepool *pool, DWORD channel, DWORD generation)
{
    if (!pool || !channel || !generation) return false;
    LONG tail = InterlockedCompareExchange(&pool->failureTail, 0, 0);
    LONG next = (tail + 1) % CHANNEL_FAILURE_COUNT;
    if (next == InterlockedCompareExchange(&pool->failureHead, 0, 0))
    {
        InterlockedExchange(&pool->backendReady, 0);
        return false;
    }
    pool->channelFailures[tail] = {channel, generation};
    MemoryBarrier();
    InterlockedExchange(&pool->failureTail, next);
    return true;
}
