#include "../audio_lease.h"
#include "../startup_lease.h"
#include <memory>
#include <cstdio>
static int checks=0,failures=0;
static void check(bool value,const char* label){++checks;printf("%s %s\n",value?"PASS":"FAIL",label);if(!value)++failures;}
int main(){
    FMOD_SYSTEM* system=nullptr;
    check(FMOD_System_Create(&system,FMOD_VERSION)==FMOD_OK,"create real FMOD");
    if(!system)return 1;
    check(FMOD_System_SetOutput(system,FMOD_OUTPUTTYPE_NOSOUND)==FMOD_OK,"no hardware device");
    check(FMOD_System_SetDSPBufferSize(system,16,2)==FMOD_OK,"production-size software buffer");
    check(FMOD_System_Init(system,32,FMOD_INIT_NORMAL,nullptr)==FMOD_OK,"init FMOD mixer");
    auto pool=std::make_unique<sharepool>();pool->magic=ASIO_PROTOCOL_MAGIC;
    FMOD_DSP* dsp=nullptr;FMOD_CHANNELGROUP* master=nullptr;
    asio::StartupLease startup(pool.get());
    check(startup.start(),"startup heartbeat starts before lease enabled");
    check(attachAudioLease(system,pool.get(),&dsp,&master)==FMOD_OK,"production lease attached");
    pool->backendReady=1;
    // Include a silent active voice so FMOD executes the DSP even when idle.
    FMOD_SOUND* sound=nullptr;FMOD_CREATESOUNDEXINFO ex={};ex.cbsize=sizeof(ex);
    ex.length=48000*sizeof(float);ex.numchannels=1;ex.defaultfrequency=48000;ex.format=FMOD_SOUND_FORMAT_PCMFLOAT;
    check(FMOD_System_CreateSound(system,nullptr,FMOD_OPENUSER|FMOD_CREATESAMPLE|FMOD_LOOP_NORMAL,&ex,&sound)==FMOD_OK,"create continuous silent fixture");
    FMOD_CHANNEL* channel=nullptr;
    if(sound)check(FMOD_System_PlaySound(system,sound,nullptr,false,&channel)==FMOD_OK,"execute DSP during simulated injection");
    for(DWORD gap:{50u,200u,300u,600u}){
        Sleep(gap);
        check(asio::recoveryState(pool.get())==RECOVERY_RUNNING,"slow injection leaves state running");
        check(pool->recoveries==0,"slow initialization causes no epoch recovery");
        check(asio::leaseFresh(pool.get(),GetTickCount()),"startup lease remains fresh");
    }
    startup.stop();
    Sleep(320);
    check(asio::recoveryState(pool.get())==RECOVERY_REQUESTED,"real post-start worker stall latches recovery");
    InterlockedExchange(&pool->heartbeat,LONG(GetTickCount()));
    check(!asio::recoveryLeaseValid(pool.get(),GetTickCount()),"refresh cannot clear real expiry");
    pool->backendReady=0;
    if(channel)FMOD_Channel_Stop(channel);
    if(sound)FMOD_Sound_Release(sound);
    if(dsp){FMOD_ChannelGroup_RemoveDSP(master,dsp);FMOD_DSP_Release(dsp);}
    FMOD_System_Release(system);
    printf("StartupLeaseProbe checks=%d failures=%d; real FMOD NOSOUND, no game injection\n",checks,failures);
    return failures?1:0;
}
