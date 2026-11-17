#pragma once
#include <windows.h>
#include "sharepool.h"

namespace asio {
inline LONG recoveryState(sharepool* pool) {
    return InterlockedCompareExchange(&pool->recoveryState,0,0);
}
inline bool requestRecovery(sharepool* pool, RecoverySource source=RECOVERY_SOURCE_UNKNOWN,
                            RecoveryStale stale=RECOVERY_STALE_NONE, DWORD age=0, DWORD now=GetTickCount()) {
    if(!pool || pool->magic!=ASIO_PROTOCOL_MAGIC || !pool->backendReady)return false;
    // Recording is already non-running/silent. Only the CAS winner writes the
    // snapshot; REQUESTED release-publishes it to the backend's stop path.
    if(InterlockedCompareExchange(&pool->recoveryState,RECOVERY_RECORDING,RECOVERY_RUNNING)!=RECOVERY_RUNNING)return false;
    InterlockedExchange(&pool->recoverySource,source);
    InterlockedExchange(&pool->recoveryStale,stale);
    InterlockedExchange(&pool->recoveryAgeMs,LONG(age));
    InterlockedExchange(&pool->recoveryEpoch,InterlockedCompareExchange(&pool->epoch,0,0));
    InterlockedExchange(&pool->recoveryTick,LONG(now));
    InterlockedExchange(&pool->recoveryState,RECOVERY_REQUESTED);
    return true;
}
inline const char* recoverySourceName(LONG source) {
    switch(source){case RECOVERY_SOURCE_DSP:return "dsp";case RECOVERY_SOURCE_BACKEND:return "backend";case RECOVERY_SOURCE_HOOK:return "hook";case RECOVERY_SOURCE_MONITOR:return "hook-heartbeat";default:return "unknown";}
}
inline const char* recoveryStaleName(LONG stale) {return stale==RECOVERY_STALE_BACKEND?"backend":stale==RECOVERY_STALE_HOOK?"hook":"none";}
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
inline bool recoveryLeaseValid(sharepool* pool,DWORD now,RecoverySource source=RECOVERY_SOURCE_UNKNOWN) {
    if(!pool || pool->magic!=ASIO_PROTOCOL_MAGIC || !pool->backendReady) return false;
    const DWORD backend=DWORD(InterlockedCompareExchange(&pool->heartbeat,0,0));
    if(!heartbeatFresh(backend,now)) {requestRecovery(pool,source,RECOVERY_STALE_BACKEND,DWORD(now-backend),now);return false;}
    const DWORD hook=DWORD(InterlockedCompareExchange(&pool->hookHeartbeat,0,0));
    if(pool->hookAttached && !heartbeatFresh(hook,now)) {requestRecovery(pool,source,RECOVERY_STALE_HOOK,DWORD(now-hook),now);return false;}
    return recoveryState(pool)==RECOVERY_RUNNING;
}
}
