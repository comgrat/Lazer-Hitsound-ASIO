// Production hook detours with deterministic BASS originals, separate TU from
// the production backend so both own their normal globals and share only pool.
#define main NativeHookTestsMain
#include "NativeHookMock.cpp"
#undef main

extern "C" sharepool* recoveryHookSetup() {
    InitializeCriticalSection(&queueLock);InitializeCriticalSection(&stateLock);
    InitializeCriticalSection(&logLock);locksInitialised=true;logLockInitialised=true;
    g_pool=new sharepool;resetFixture();
    return MyPool;
}
extern "C" DWORD recoveryHookCreate() {
    auto f=createChannel(true,0.25f,false);
    MyBASS_ChannelFlags(f.channel,BASS_SAMPLE_LOOP,BASS_SAMPLE_LOOP);
    return f.channel;
}
extern "C" bool recoveryHookSampleReady() {
    EnterCriticalSection(&stateLock);
    bool ready=sampleReady(g_mock.sampleHandle);
    LeaveCriticalSection(&stateLock);return ready;
}
extern "C" void recoveryHookStart(DWORD channel,bool restart) {
    setGameVolume(channel,0.5f);
    AsioChannelContextV2(channel,channelGeneration(channel),0.25f,1);
    MyBASS_ChannelPlay(channel,restart?TRUE:FALSE);
}
extern "C" void recoveryHookPoll() {std::vector<MonitorSnapshot> pending;monitorChannelsOnce(pending);}
extern "C" bool recoveryHookNative(DWORD channel) {return routeIs(channel,asio::Route::Native);}
extern "C" float recoveryHookVolume(DWORD channel) {return g_mock.nativeVolume[channel];}
extern "C" void recoveryHookRestoreFails(bool fail) {g_mock.setAttributeOk=!fail;}
extern "C" void recoveryHookFree(DWORD channel) {MyBASS_StreamFree(channel);}
extern "C" DWORD recoveryHookGeneration(DWORD channel) {return channelGeneration(channel);}
extern "C" void recoveryHookFinish() {
    for(auto &entry:sampleInfoMap)if(entry.second.mappingHandle)CloseHandle(entry.second.mappingHandle);
    sampleInfoMap.clear();channelInfoMap.clear();
}
