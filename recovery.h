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
inline bool leaseFresh(const sharepool* pool,DWORD now) {
    return pool && pool->magic==ASIO_PROTOCOL_MAGIC && pool->backendReady &&
        DWORD(now-DWORD(pool->heartbeat))<250 &&
        (!pool->hookAttached || DWORD(now-DWORD(pool->hookHeartbeat))<250);
}
// Expiry latches the output off. Refreshing heartbeats cannot undo this CAS.
// No allocations, locks, I/O or FMOD calls: safe in the lease DSP callback.
inline bool recoveryLeaseValid(sharepool* pool,DWORD now) {
    if(!pool || pool->magic!=ASIO_PROTOCOL_MAGIC || !pool->backendReady) return false;
    if(!leaseFresh(pool,now)) {requestRecovery(pool);return false;}
    return recoveryState(pool)==RECOVERY_RUNNING;
}
}
