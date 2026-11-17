#pragma once
#include <windows.h>
#include "sharepool.h"

namespace asio {
inline LONG recoveryState(sharepool* pool) {
    return InterlockedCompareExchange(&pool->recoveryState,0,0);
}
inline void requestRecovery(sharepool* pool) {
    if(pool && pool->magic==ASIO_PROTOCOL_MAGIC && pool->backendReady)
        InterlockedCompareExchange(&pool->recoveryState,RECOVERY_REQUESTED,RECOVERY_RUNNING);
}
// A reader samples now before it loads another thread's heartbeat. A concurrent
// publication can therefore be slightly ahead of now; unsigned subtraction must
// not turn that small lead into a 49-day-old timestamp. Bound both directions
// to one lease interval, preserving actual expiry and rejecting arbitrary future
// values. DWORD subtraction also handles the 32-bit GetTickCount wrap.
inline bool heartbeatFresh(DWORD heartbeat,DWORD now) {
    return DWORD(now-heartbeat)<250 || DWORD(heartbeat-now)<250;
}
inline bool leaseFresh(const sharepool* pool,DWORD now) {
    return pool && pool->magic==ASIO_PROTOCOL_MAGIC && pool->backendReady &&
        heartbeatFresh(DWORD(pool->heartbeat),now) &&
        (!pool->hookAttached || heartbeatFresh(DWORD(pool->hookHeartbeat),now));
}
// Expiry latches the output off. Refreshing heartbeats cannot undo this CAS.
// No allocations, locks, I/O or FMOD calls: safe in the lease DSP callback.
inline bool recoveryLeaseValid(sharepool* pool,DWORD now) {
    if(!pool || pool->magic!=ASIO_PROTOCOL_MAGIC || !pool->backendReady) return false;
    if(!leaseFresh(pool,now)) {requestRecovery(pool);return false;}
    return recoveryState(pool)==RECOVERY_RUNNING;
}
}
