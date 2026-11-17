#pragma once
#include <windows.h>
#include "sharepool.h"

namespace asio {
// Main is not the event worker during injection/managed attach. Keep the lease
// alive across ALL startup blocking operations, including loader/console waits.
// Stop before the event worker starts: this must never hide a runtime stall.
class StartupLease {
    sharepool* pool;
    HANDLE stopEvent=nullptr,thread=nullptr;
    static DWORD WINAPI run(void* value) {
        auto* self=static_cast<StartupLease*>(value);
        do {
            InterlockedExchange(&self->pool->heartbeat,LONG(GetTickCount()));
        } while(WaitForSingleObject(self->stopEvent,10)==WAIT_TIMEOUT);
        return 0;
    }
public:
    explicit StartupLease(sharepool* value):pool(value) {}
    bool start() {
        if(thread || !pool)return false;
        stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopEvent)return false;
        InterlockedExchange(&pool->heartbeat,LONG(GetTickCount()));
        thread=CreateThread(nullptr,0,run,this,0,nullptr);
        if(!thread){CloseHandle(stopEvent);stopEvent=nullptr;return false;}
        return true;
    }
    void stop() {
        if(!thread)return;
        SetEvent(stopEvent);
        WaitForSingleObject(thread,INFINITE);
        CloseHandle(thread);CloseHandle(stopEvent);thread=nullptr;stopEvent=nullptr;
        InterlockedExchange(&pool->heartbeat,LONG(GetTickCount()));
    }
    ~StartupLease(){stop();}
    StartupLease(const StartupLease&)=delete;
    StartupLease& operator=(const StartupLease&)=delete;
};
}
