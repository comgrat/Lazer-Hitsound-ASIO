#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <winuser.h>
#include "fmod/inc/fmod.h"
#include "fmod/inc/fmod_errors.h"
using namespace std;
FMOD_SYSTEM *fmodSystem;
int bufferSize = 128, driverId = 0, sampleRate = 48000, outputVolume = 100, debugLogEnabled = 0;

int readValue(FILE *fp, int defaultValue){
	int x=0,f=1;int raw=fgetc(fp); if(raw==EOF) return defaultValue; char ch=(char)raw;
	while(ch<'0'||ch>'9'){if(ch=='-')f=-1; raw=fgetc(fp); if(raw==EOF) return defaultValue; ch=(char)raw;}
	while(ch>='0'&&ch<='9'){x=x*10+ch-'0'; raw=fgetc(fp); if(raw==EOF) break; ch=(char)raw;}
	return x*f;
}

void init(bool forceWrite = false){
	FILE *fp;
	if(forceWrite || !(fp = fopen("config.ini", "r"))){
		fp = fopen("config.ini", "w");
		fprintf(fp, "[Config]\n");
		fprintf(fp, "Buffer Size = %d\n", bufferSize);
		fprintf(fp, "ASIO Driver ID = %d\n", driverId);
		fprintf(fp, "Sample Rate = %d\n", sampleRate);
		fprintf(fp, "Output Volume = %d\n", outputVolume);
		fprintf(fp, "Debug Log = %d\n", debugLogEnabled);
		fclose(fp);
	}else{
		bufferSize = readValue(fp, bufferSize);
		driverId = readValue(fp, driverId);
		sampleRate = readValue(fp, sampleRate);
		outputVolume = readValue(fp, outputVolume);
		debugLogEnabled = readValue(fp, debugLogEnabled);
		if(outputVolume < 0) outputVolume = 0;
		debugLogEnabled = debugLogEnabled ? 1 : 0;
		fclose(fp);
	}
}

int main(int argc, char* argv[]) {
    printf("FMOD Studio Low Level API (C) Firelight Technologies Pty Ltd.\n");
    printf("For custom key bindings, refer keycodes on site:\n");
    printf("    https://docs.microsoft.com/en-us/windows/desktop/inputdev/virtual-key-codes\n");

    FMOD_RESULT initRet = FMOD_System_Create(&fmodSystem, FMOD_VERSION);
    if (initRet != FMOD_OK) {
        printf("Create FMOD System Failed: %s\n", FMOD_ErrorString((FMOD_RESULT)initRet));
        return initRet;
    }
    
    printf("Please input buffer size (exp. 128): ");
	bufferSize = readValue(stdin, bufferSize);

    FMOD_System_SetDSPBufferSize(fmodSystem, bufferSize, 2);
	FMOD_System_SetOutput(fmodSystem, FMOD_OUTPUTTYPE_ASIO);
	int driverNums;
	FMOD_System_GetNumDrivers(fmodSystem, &driverNums);
	char name[256];
	int systemRate;
	int speakerChannels;
	FMOD_SPEAKERMODE speakerMode;

	for (int i = 0; i < driverNums; i++) {
		FMOD_System_GetDriverInfo(fmodSystem, i, name, 255, 0, &systemRate, &speakerMode, &speakerChannels);
		printf("DeviceID: %-3d DeviceName: %s  Rate: %d  Channels: %d\n", i, name, systemRate, speakerChannels);
	}
	printf("Please select the DeviceID: ");
	driverId = readValue(stdin, driverId);
	FMOD_System_SetDriver(fmodSystem, driverId);

	unsigned bufLen;
	int bufNum;
	
	FMOD_System_GetDriverInfo(fmodSystem, driverId, name, 255, 0, &systemRate, &speakerMode, &speakerChannels);
	printf("Please input sample rate (input 0 for %dhz): ", systemRate);
	int tmpSysRate = readValue(stdin, sampleRate);
	sampleRate = (tmpSysRate == 0 ? systemRate : tmpSysRate);
	printf("Please input output volume percent (exp. 100): ");
	outputVolume = readValue(stdin, outputVolume);
	if(outputVolume < 0) outputVolume = 0;
	printf("Enable debug log? (0/1): ");
	debugLogEnabled = readValue(stdin, debugLogEnabled);
	debugLogEnabled = debugLogEnabled ? 1 : 0;

	if (speakerChannels <= 0) speakerChannels = 2;
	FMOD_System_SetSoftwareFormat(fmodSystem, sampleRate, speakerMode, speakerChannels);

	initRet = FMOD_System_Init(fmodSystem, 32, FMOD_INIT_NORMAL, 0);
	if (initRet != FMOD_OK) {
		printf("FMOD System Initialize Failed: %s\n", FMOD_ErrorString((FMOD_RESULT)initRet));
		system("pause");
		return 0;
	}
	Sleep(1000);
	FMOD_System_GetDriverInfo(fmodSystem, driverId, name, 255, 0, 0, &speakerMode, &speakerChannels);
	FMOD_System_GetDSPBufferSize(fmodSystem, &bufLen, &bufNum);
	printf("FMOD System Initialize Finished.\n");
	printf("[FMOD] Device Name: %s\n", name);
	printf("[FMOD] Device Sample Rate: %d\n", sampleRate);
	printf("[FMOD] Device Speaker Mode: %d\n", (int)speakerMode);
	printf("[FMOD] Device Channels: %d\n", speakerChannels);
	printf("[FMOD] DSP buffer size: %d * %d\n", bufLen, bufNum);
	printf("[FMOD] Latency: %.10lfms\n", bufLen * bufNum * 1000.0 / sampleRate);

    if (initRet == FMOD_OK && fmodSystem != nullptr) {
        FMOD_System_Release(fmodSystem);
        printf("FMOD System released.\n");
    }
    
    init(true);
    
    system("pause");

    return 0;
}
