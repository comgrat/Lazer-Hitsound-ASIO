#define main RecoveryBackendEntry
#include "../osu!asio_sound.cpp"
#undef main
#include <functional>

extern "C" sharepool* recoveryHookSetup();
extern "C" DWORD recoveryHookCreate();
extern "C" bool recoveryHookSampleReady();
extern "C" void recoveryHookStart(DWORD,bool);
extern "C" void recoveryHookPoll();
extern "C" bool recoveryHookNative(DWORD);
extern "C" float recoveryHookVolume(DWORD);
extern "C" void recoveryHookRestoreFails(bool);
extern "C" void recoveryHookFree(DWORD);
extern "C" DWORD recoveryHookGeneration(DWORD);
extern "C" void recoveryHookFinish();
static int checks=0;
static void require(bool value,const char* message) {
    ++checks;printf("%s %s\n",value?"PASS":"FAIL",message);
    if(!value)throw std::runtime_error(message);
}
static bool eventually(const std::function<bool()>& test) {
    auto end=GetTickCount64()+3000;
    do {if(test())return true;Sleep(1);}while(GetTickCount64()<end);
    return false;
}
static int voices(){int n=0;FMOD_System_GetChannelsPlaying(fmodSystem,&n,nullptr);return n;}
int main() {
    HANDLE worker=nullptr;FMOD_DSP* lease=nullptr;
    try {
        MyPool=recoveryHookSetup();
        require(FMOD_System_Create(&fmodSystem,FMOD_VERSION)==FMOD_OK,"create FMOD");
        require(FMOD_System_SetOutput(fmodSystem,FMOD_OUTPUTTYPE_NOSOUND)==FMOD_OK,"no hardware output");
        require(FMOD_System_Init(fmodSystem,32,FMOD_INIT_NORMAL,nullptr)==FMOD_OK,"init FMOD");
        require(attachAudioLease(fmodSystem,MyPool,&lease,&asioOutputGroup)==FMOD_OK,"production lease DSP attached");
        outputVolume=100;requestedOutputVolume=100;requestedDebugLog=0;mainThreadId=GetCurrentThreadId();
        MSG m={};PeekMessage(&m,nullptr,0,0,PM_NOREMOVE);
        Work=true;worker=CreateThread(nullptr,0,mainloop,nullptr,0,nullptr);
        require(worker!=nullptr,"production worker running");
        DWORD channel=recoveryHookCreate();
        require(channel!=0,"production hook published actual PCM mapping");
        require(eventually([]{return recoveryHookSampleReady();}),"backend ACK from real FMOD sample load");
        recoveryHookStart(channel,false);
        require(eventually([]{return voices()==1;}),"external loop playing");
        require(recoveryHookVolume(channel)==0,"native loop suppressed");
        for(int cycle=0;cycle<2;++cycle) {
            DWORD oldGeneration=recoveryHookGeneration(channel);
            // Pause the actual consumer thread; always resume before assertions.
            DWORD suspended=SuspendThread(worker);
            if(suspended==DWORD(-1))throw std::runtime_error("SuspendThread failed");
            Sleep(300);
            recoveryHookPoll();
            LONG phase=asio::recoveryState(MyPool);
            float volume=recoveryHookVolume(channel);
            ResumeThread(worker);
            require(phase==RECOVERY_REQUESTED,"stalled consumer latches recovery");
            require(volume==0,"native remains muted until old voice stop ACK");
            require(eventually([]{return asio::recoveryState(MyPool)==RECOVERY_BACKEND_STOPPED;}),"backend acknowledges old epoch stopped");
            require(eventually([]{return voices()==0;}),"no old voices before restoration");
            recoveryHookPoll();
            require(recoveryHookNative(channel)&&recoveryHookVolume(channel)==0.5f,"hook restores native after stop");
            require(eventually([]{return asio::recoveryState(MyPool)==RECOVERY_RUNNING;}),"both production sides finish replay handshake");
            require(recoveryHookSampleReady(),"sample republished and acknowledged");
            recoveryHookStart(channel,false);
            require(recoveryHookNative(channel)&&voices()==0,"old lifetime cannot reacquire ASIO");
            if(cycle==0) {
                recoveryHookFree(channel);channel=recoveryHookCreate();
            }
            recoveryHookStart(channel,cycle==1);
            require(recoveryHookGeneration(channel)!=oldGeneration,"new allocation or explicit restart advances lifetime");
            require(eventually([]{return voices()==1;}),"new lifetime uses replayed sample");
        }
        // Producer/monitor stall independently of a healthy consumer.
        MyPool->hookAttached=1;MyPool->hookHeartbeat=LONG(GetTickCount()-300);
        require(eventually([]{return asio::recoveryState(MyPool)==RECOVERY_BACKEND_STOPPED;}),"producer lease expiry stops backend independently");
        recoveryHookRestoreFails(true);recoveryHookPoll();
        require(!MyPool->backendReady,"failed native restore permanently disables recovery");
        recoveryHookRestoreFails(false);recoveryHookPoll();
        require(!MyPool->backendReady,"successful later native retry cannot reopen ASIO");
        require(eventually([&]{return WaitForSingleObject(worker,0)==WAIT_OBJECT_0;}),"permanent fault exits worker");
        Work=false;CloseHandle(worker);worker=nullptr;
        recoveryHookFinish();FMOD_System_Release(fmodSystem);fmodSystem=nullptr;
        printf("RecoveryIntegration checks=%d failures=0; production hook/backend, mock BASS, real FMOD NOSOUND\n",checks);
        return 0;
    }catch(const std::exception& e) {
        printf("FAIL %s\n",e.what());Work=false;
        if(worker){WaitForSingleObject(worker,3000);CloseHandle(worker);}
        return 1;
    }
}
