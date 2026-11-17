#pragma once
#include <windows.h>
#include "sharepool.h"
namespace asio {
enum NativeCallId : LONG { NativeUnknown=0,NativePlay,NativePause,NativeStop,NativeActive,NativeAttribute,NativeFlags,NativeMixerAdd,NativeMixerFlags,NativeMixerRemove,NativeStreamFree,NativeSampleLoad,NativeSampleFree,NativeSampleChannel,NativeSampleInfo,NativeSampleData };
inline const char* nativeCallName(LONG id) {
    const char* names[]={"none","ChannelPlay","ChannelPause","ChannelStop","ChannelIsActive","ChannelSetAttribute","ChannelFlags","Mixer_StreamAddChannel","Mixer_ChannelFlags","Mixer_ChannelRemove","StreamFree","SampleLoad","SampleFree","SampleGetChannel","SampleGetInfo","SampleGetData"};
    return id>=0 && id<LONG(sizeof(names)/sizeof(names[0]))?names[id]:"unknown";
}
inline LONGLONG lockClock(){LARGE_INTEGER value;QueryPerformanceCounter(&value);return value.QuadPart;}
inline LONG elapsedUs(LONGLONG begin){LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);auto elapsed=(lockClock()-begin)*1000000/frequency.QuadPart;return elapsed>MAXLONG?MAXLONG:LONG(elapsed);}
inline void recordMax(volatile LONG* field,LONG value){LONG old=InterlockedCompareExchange(field,0,0);while(value>old){LONG found=InterlockedCompareExchange(field,value,old);if(found==old)return;old=found;}}
struct StateLockTiming {
    unsigned depth=0;LONGLONG begin=0;bool timing=false;
    void enter(CRITICAL_SECTION* lock,sharepool* pool,bool enabled){
        bool outer=depth==0;LONGLONG wait=outer&&enabled?lockClock():0;
        EnterCriticalSection(lock);
        if(outer){timing=enabled;begin=enabled?lockClock():0;if(enabled&&pool)recordMax(&pool->stateLockWaitMaxUs,elapsedUs(wait));}
        ++depth;
    }
    void leave(CRITICAL_SECTION* lock,sharepool* pool){
        if(depth && --depth==0){if(timing&&pool)recordMax(&pool->stateLockHoldMaxUs,elapsedUs(begin));timing=false;}
        LeaveCriticalSection(lock);
    }
};
}
