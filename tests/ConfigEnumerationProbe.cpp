// Execute the real --list enumeration with a deterministic empty ASIO list.
#include "../fmod/inc/fmod.h"
#include <cstdio>

static int openCalls=0, releaseCalls=0;
static bool enumerationFails=false;
extern "C" {
FMOD_RESULT F_API FMOD_System_Create(FMOD_SYSTEM** system,unsigned int) { *system=reinterpret_cast<FMOD_SYSTEM*>(1);return FMOD_OK; }
FMOD_RESULT F_API FMOD_System_Release(FMOD_SYSTEM*) { ++releaseCalls;return FMOD_OK; }
FMOD_RESULT F_API FMOD_System_SetOutput(FMOD_SYSTEM*,FMOD_OUTPUTTYPE) { return FMOD_OK; }
FMOD_RESULT F_API FMOD_System_GetNumDrivers(FMOD_SYSTEM*,int* count) { *count=0;return enumerationFails?FMOD_ERR_INTERNAL:FMOD_OK; }
FMOD_RESULT F_API FMOD_System_GetDriverInfo(FMOD_SYSTEM*,int,char*,int,FMOD_GUID*,int*,FMOD_SPEAKERMODE*,int*) { return FMOD_ERR_INVALID_PARAM; }
FMOD_RESULT F_API FMOD_System_Init(FMOD_SYSTEM*,int,FMOD_INITFLAGS,void*) { ++openCalls;return FMOD_ERR_INTERNAL; }
}
#include "../gui/audio_device.h"

int main() {
    asio::i18n::initialize();
    int empty=lazer::listDevicesCommand();
    std::vector<lazer::AsioDevice> devices;
    FMOD_RESULT listed=lazer::enumerateDevices(devices);
    enumerationFails=true;
    int error=lazer::listDevicesCommand();
    bool ok=empty==0&&error==1&&listed==FMOD_OK&&devices.empty()&&openCalls==0&&releaseCalls==3;
    std::printf("ConfigEnumerationProbe: empty=%d error=%d deviceOpens=%d releases=%d %s\n",empty,error,openCalls,releaseCalls,ok?"PASS":"FAIL");
    return ok?0:1;
}
