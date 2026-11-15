// Read the actual FMOD mixed output without opening ASIO/WASAPI. The production
// PCM loader/playback helpers and lease DSP are used, with a synthetic impulse.
#define main AsioBackendProgramMain
#include "../osu!asio_sound.cpp"
#undef main
#include <memory>
#include <algorithm>
#include <io.h>
#include <fcntl.h>

static std::vector<float> capture;
static FMOD_RESULT F_CALL readCapture(FMOD_DSP_STATE*, float* in, float* out, unsigned length, int channels, int* outChannels) {
    *outChannels=channels;
    if(in){if(out!=in)memcpy(out,in,size_t(length)*channels*sizeof(float));}
    else memset(out,0,size_t(length)*channels*sizeof(float));
    // NOSOUND_NRT processes synchronously in System_Update on this test thread.
    for(unsigned frame=0;frame<length;++frame){float peak=0;for(int ch=0;ch<channels;++ch)peak=(std::max)(peak,std::fabs(out[frame*channels+ch]));capture.push_back(peak);}
    return FMOD_OK;
}
static int failures=0,checks=0;
static void check(bool value,const char* text){++checks;if(!value){++failures;printf("FAIL: %s\n",text);}}
int main(){
    check(FMOD_System_Create(&fmodSystem,FMOD_VERSION)==FMOD_OK,"create");
    if(!fmodSystem)return 1;
    check(FMOD_System_SetOutput(fmodSystem,FMOD_OUTPUTTYPE_NOSOUND_NRT)==FMOD_OK,"non-realtime no-device output");
    check(FMOD_System_SetDSPBufferSize(fmodSystem,16,2)==FMOD_OK,"same 16x2 software buffer");
    check(FMOD_System_SetSoftwareFormat(fmodSystem,48000,FMOD_SPEAKERMODE_STEREO,0)==FMOD_OK,"stereo");
    check(FMOD_System_Init(fmodSystem,16,FMOD_INIT_NORMAL,nullptr)==FMOD_OK,"init");
    auto pool=std::make_unique<sharepool>();MyPool=pool.get();pool->magic=ASIO_PROTOCOL_MAGIC;pool->backendReady=1;pool->heartbeat=LONG(GetTickCount());
    FMOD_DSP* lease=nullptr;
    check(attachAudioLease(fmodSystem,MyPool,&lease,&asioOutputGroup)==FMOD_OK,"production lease attached");
    FMOD_DSP_DESCRIPTION desc={};desc.pluginsdkversion=FMOD_PLUGIN_SDK_VERSION;strcpy_s(desc.name,"Test capture");desc.version=1;desc.numinputbuffers=desc.numoutputbuffers=1;desc.read=readCapture;
    FMOD_DSP* tap=nullptr;
    check(FMOD_System_CreateDSP(fmodSystem,&desc,&tap)==FMOD_OK&&FMOD_ChannelGroup_AddDSP(asioOutputGroup,FMOD_CHANNELCONTROL_DSP_HEAD,tap)==FMOD_OK,"digital tap");
    // A 1kHz burst of 32 frames at 40ms, followed by silence. An echo would be
    // another nonzero island outside this expected single bounded response.
    SampleData data;data.sampleRate=48000;data.channels=1;data.format=SAMPLE_FORMAT_FLOAT;
    std::vector<float> pcm(4800,0);
    for(unsigned i=1920;i<1952;++i)pcm[i]=0.1f*std::sin(6.28318530718f*(i-1920)/48.0f);
    FMOD_CREATESOUNDEXINFO info={};info.cbsize=sizeof(info);info.length=DWORD(pcm.size()*sizeof(float));info.numchannels=1;info.defaultfrequency=48000;info.format=FMOD_SOUND_FORMAT_PCMFLOAT;
    check(FMOD_System_CreateSound(fmodSystem,reinterpret_cast<const char*>(pcm.data()),FMOD_OPENMEMORY|FMOD_OPENRAW|FMOD_CREATESAMPLE,&info,&data.sound)==FMOD_OK,"synthetic one-burst PCM");
    for(int percent:{0,25,50,100}){
        FMOD_ChannelGroup_SetVolumeRamp(asioOutputGroup,false);
        outputVolume=percent;requestedOutputVolume=percent;requestedDebugLog=0;
        check(FMOD_ChannelGroup_SetVolume(asioOutputGroup,percent/100.0f)==FMOD_OK,"independent bus volume");
        capture.clear();
        ChannelData voice;voice.sample=9;voice.generation=1;voice.volume=0.6f;voice.frequency=48000;
        auto before=nextVoiceId;
        ensurePlaying(voice,data);
        ensurePlaying(voice,data); // Same logical request must not add a second voice.
        check(nextVoiceId==before+1,"duplicate request is idempotent");
        unsigned iterations=0;
        for(;iterations<2000;++iterations){pool->heartbeat=LONG(GetTickCount());check(FMOD_System_Update(fmodSystem)==FMOD_OK,"render update");if(capture.size()>14000)break;}
        float peak=0;std::vector<size_t> active;
        for(size_t i=0;i<capture.size();++i){peak=(std::max)(peak,capture[i]);if(capture[i]>0.00001f)active.push_back(i);}
        size_t first=active.empty()?0:active.front(),last=active.empty()?0:active.back();
        printf("render percent=%d frames=%zu peak=%.8f first=%zu last=%zu span=%zu voice=%llu\n",percent,capture.size(),peak,first,last,active.empty()?0:last-first+1,voice.voiceId);
        check(percent==0?active.empty():!active.empty(),"zero mutes and nonzero renders");
        if(percent){check(last-first<128,"single burst has no delayed second response in FMOD output");check(peak<0.1f*0.6f*percent/100.0f+0.0001f,"gain never multiplied twice or duplicated");}
        // Ended voice must not be restarted by an attribute change / late PLAY.
        FMOD_Channel_Stop(voice.channel);applyChannelState(voice);
        auto id=nextVoiceId;
        ensurePlaying(voice,data);
        check(nextVoiceId==id,"finished generation cannot re-create a voice");
    }
    // The diagnostic sink is deliberately blocked on a tiny unread pipe.
    // Playback submissions must not wait for diagnostic I/O, even with Debug1.
    HANDLE pipeRead=nullptr,pipeWrite=nullptr;
    check(CreatePipe(&pipeRead,&pipeWrite,nullptr,1024)!=FALSE,"blocked diagnostic pipe");
    FILE* pipeSink=nullptr;
    if(pipeWrite){int descriptor=_open_osfhandle(reinterpret_cast<intptr_t>(pipeWrite),_O_BINARY);pipeSink=_fdopen(descriptor,"wb");}
    playbackLog=new asio::AsyncTextLog;
    check(pipeSink&&playbackLog->start(pipeSink),"async diagnostic sink starts");
    if(pipeSink){
        debugLogEnabled=1;
        for(unsigned i=0;i<9000;++i)debugPrintf("pipe-pressure-%u 12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890\n",i);
        Sleep(20);
        LARGE_INTEGER begin={},end={},frequency={};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&begin);
        ChannelData left;left.sample=21;left.handle=21;left.generation=1;left.volume=0.6f;
        ChannelData right;right.sample=22;right.handle=22;right.generation=2;right.volume=0.6f;
        ensurePlaying(left,data);ensurePlaying(right,data);
        QueryPerformanceCounter(&end);
        double ms=1000.0*double(end.QuadPart-begin.QuadPart)/frequency.QuadPart;
        printf("two legal layers with blocked logger: %.3fms, diagnostic lines dropped=%lu\n",ms,playbackLog->dropped());
        check(left.channel&&right.channel&&left.voiceId!=right.voiceId,"two legal layers preserved under log pressure");
        check(ms<20.0,"blocked diagnostic sink cannot serialize the two layer submissions for tens of ms");
        check(playbackLog->dropped()>0,"diagnostic overflow drops logs instead of blocking sounds");
        FMOD_Channel_Stop(left.channel);FMOD_Channel_Stop(right.channel);
        CloseHandle(pipeRead);pipeRead=nullptr;
        check(playbackLog->stop(),"logger exits after sink unblocks");
        fclose(pipeSink);
    }
    if(pipeRead)CloseHandle(pipeRead);
    debugLogEnabled=0;
    FMOD_Sound_Release(data.sound);pool->backendReady=0;
    FMOD_ChannelGroup_RemoveDSP(asioOutputGroup,tap);FMOD_DSP_Release(tap);
    FMOD_ChannelGroup_RemoveDSP(asioOutputGroup,lease);FMOD_DSP_Release(lease);
    FMOD_System_Release(fmodSystem);fmodSystem=nullptr;MyPool=nullptr;
    printf("DigitalOutputProbe: checks=%d failures=%d; no hardware/acoustic claim\n",checks,failures);
    return failures?1:0;
}
