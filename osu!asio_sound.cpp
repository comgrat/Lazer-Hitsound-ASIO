#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <unordered_map>

#include <windows.h>
#include <winuser.h>
#include <TlHelp32.h>

#include "fmod/inc/fmod.h"
#include "fmod/inc/fmod_errors.h"
#include "sharepool.h"

using namespace std;

struct SampleData
{
    FMOD_SOUND *sound;
    vector<char> pcm;
    DWORD sampleRate;
    DWORD channels;
    DWORD format;
    bool looping;

    SampleData()
        : sound(nullptr), sampleRate(0), channels(0), format(SAMPLE_FORMAT_UNKNOWN), looping(false)
    {
    }
};

struct ChannelData
{
    HSAMPLE sample;
    FMOD_CHANNEL *channel;
    float volume;
    float pan;
    float frequency;
    bool looping;
    bool playing;

    ChannelData()
        : sample(0), channel(nullptr), volume(1.0f), pan(0.0f), frequency(0.0f), looping(false), playing(false)
    {
    }
};

static FMOD_SYSTEM *fmodSystem = nullptr;
static struct sharepool *MyPool = nullptr;
static volatile bool Work = false;

static int bufferSize = 128;
static int driverId = 0;
static int sampleRate = 48000;
static int outputVolume = 100;
static int debugLogEnabled = 0;
static FMOD_SPEAKERMODE driverSpeakerMode = FMOD_SPEAKERMODE_DEFAULT;
static int driverSpeakerChannels = 0;

static void debugPrintf(const char *format, ...)
{
    if (!debugLogEnabled)
        return;

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static int readValue(FILE *fp, int defaultValue)
{
    int x = 0, f = 1;
    int raw = fgetc(fp);
    if (raw == EOF)
        return defaultValue;
    char ch = (char)raw;
    while (ch < '0' || ch > '9')
    {
        if (ch == '-')
            f = -1;
        raw = fgetc(fp);
        if (raw == EOF)
            return defaultValue;
        ch = (char)raw;
    }
    while (ch >= '0' && ch <= '9')
    {
        x = x * 10 + ch - '0';
        raw = fgetc(fp);
        if (raw == EOF)
            break;
        ch = (char)raw;
    }
    return x * f;
}

static void init(bool forceWrite = false)
{
    FILE *fp;
    if (forceWrite || !(fp = fopen("config.ini", "r")))
    {
        fp = fopen("config.ini", "w");
        fprintf(fp, "[Config]\n");
        fprintf(fp, "Buffer Size = %d\n", bufferSize);
        fprintf(fp, "ASIO Driver ID = %d\n", driverId);
        fprintf(fp, "Sample Rate = %d\n", sampleRate);
        fprintf(fp, "Output Volume = %d\n", outputVolume);
        fprintf(fp, "Debug Log = %d\n", debugLogEnabled);
        fclose(fp);
    }
    else
    {
        bufferSize = readValue(fp, bufferSize);
        driverId = readValue(fp, driverId);
        sampleRate = readValue(fp, sampleRate);
        outputVolume = readValue(fp, outputVolume);
        debugLogEnabled = readValue(fp, debugLogEnabled);
        if (outputVolume < 0)
            outputVolume = 0;
        debugLogEnabled = debugLogEnabled ? 1 : 0;
        fclose(fp);
    }
}

static FMOD_SOUND_FORMAT toFmodFormat(DWORD format)
{
    switch (format)
    {
    case SAMPLE_FORMAT_PCM8:
        return FMOD_SOUND_FORMAT_PCM8;
    case SAMPLE_FORMAT_PCM16:
        return FMOD_SOUND_FORMAT_PCM16;
    case SAMPLE_FORMAT_PCM24:
        return FMOD_SOUND_FORMAT_PCM24;
    case SAMPLE_FORMAT_PCM32:
        return FMOD_SOUND_FORMAT_PCM32;
    case SAMPLE_FORMAT_FLOAT:
        return FMOD_SOUND_FORMAT_PCMFLOAT;
    default:
        return FMOD_SOUND_FORMAT_NONE;
    }
}

static void applyChannelState(ChannelData &channelData)
{
    if (channelData.channel == nullptr)
        return;

    FMOD_Channel_SetVolume(channelData.channel, outputVolume / 100.0f);
    FMOD_Channel_SetPan(channelData.channel, channelData.pan);

    if (channelData.frequency > 0)
        FMOD_Channel_SetFrequency(channelData.channel, channelData.frequency);

    FMOD_Channel_SetMode(channelData.channel, channelData.looping ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF);
}

static bool loadSampleData(const SampleLoadedPayload &payload, unordered_map<HSAMPLE, SampleData> &samples)
{
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, payload.mappingName);
    if (mapping == NULL)
        return false;

    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, payload.dataLength);
    if (view == NULL)
    {
        CloseHandle(mapping);
        return false;
    }

    SampleData sampleData;
    sampleData.pcm.resize(payload.dataLength);
    memcpy(sampleData.pcm.data(), view, payload.dataLength);
    sampleData.sampleRate = payload.sampleRate;
    sampleData.channels = payload.channels;
    sampleData.format = payload.format;

    UnmapViewOfFile(view);
    CloseHandle(mapping);

    FMOD_CREATESOUNDEXINFO exinfo;
    memset(&exinfo, 0, sizeof(exinfo));
    exinfo.cbsize = sizeof(exinfo);
    exinfo.length = payload.dataLength;
    exinfo.numchannels = payload.channels;
    exinfo.defaultfrequency = payload.sampleRate;
    exinfo.format = toFmodFormat(payload.format);

    if (exinfo.format == FMOD_SOUND_FORMAT_NONE)
        return false;

    FMOD_RESULT result = FMOD_System_CreateSound(
        fmodSystem,
        sampleData.pcm.data(),
        FMOD_OPENMEMORY | FMOD_OPENRAW | FMOD_CREATESAMPLE | FMOD_LOWMEM | FMOD_LOOP_OFF,
        &exinfo,
        &sampleData.sound);

    if (result != FMOD_OK)
    {
        debugPrintf("[FMOD] CreateSound failed: %s\n", FMOD_ErrorString(result));
        return false;
    }

    auto existing = samples.find(payload.sample);
    if (existing != samples.end() && existing->second.sound != nullptr)
        FMOD_Sound_Release(existing->second.sound);

    samples[payload.sample] = sampleData;
    return true;
}

static void removeSample(HSAMPLE sample, unordered_map<HSAMPLE, SampleData> &samples, unordered_map<HCHANNEL, ChannelData> &channels)
{
    for (auto it = channels.begin(); it != channels.end();)
    {
        if (it->second.sample == sample)
        {
            if (it->second.channel != nullptr)
                FMOD_Channel_Stop(it->second.channel);
            it = channels.erase(it);
        }
        else
            ++it;
    }

    auto sampleIt = samples.find(sample);
    if (sampleIt != samples.end())
    {
        if (sampleIt->second.sound != nullptr)
            FMOD_Sound_Release(sampleIt->second.sound);
        samples.erase(sampleIt);
    }
}

static void ensurePlaying(ChannelData &channelData, SampleData &sampleData)
{
    if (sampleData.sound == nullptr)
        return;

    if (channelData.channel == nullptr)
    {
        FMOD_CHANNEL *newChannel = nullptr;
        FMOD_RESULT result = FMOD_System_PlaySound(fmodSystem, sampleData.sound, nullptr, true, &newChannel);
        if (result != FMOD_OK)
        {
            debugPrintf("[FMOD] PlaySound failed: %s\n", FMOD_ErrorString(result));
            return;
        }

        channelData.channel = newChannel;
        applyChannelState(channelData);
        debugPrintf("[FMOD] Channel created for sample %lu\n", (unsigned long)channelData.sample);
    }

    FMOD_RESULT result = FMOD_Channel_SetPaused(channelData.channel, false);
    if (result != FMOD_OK)
    {
        debugPrintf("[FMOD] SetPaused(false) failed: %s\n", FMOD_ErrorString(result));
        return;
    }

    channelData.playing = true;
    FMOD_BOOL paused = false;
    FMOD_BOOL isPlaying = false;
    float audibility = 0.0f;
    FMOD_Channel_GetPaused(channelData.channel, &paused);
    FMOD_Channel_IsPlaying(channelData.channel, &isPlaying);
    FMOD_Channel_GetAudibility(channelData.channel, &audibility);
    debugPrintf("[FMOD] Channel %lu playing paused=%d isPlaying=%d audibility=%.4f\n",
                (unsigned long)channelData.sample,
                paused,
                isPlaying,
                audibility);
}

static void ensurePaused(ChannelData &channelData)
{
    if (channelData.channel != nullptr)
    {
        FMOD_RESULT result = FMOD_Channel_SetPaused(channelData.channel, true);
        if (result != FMOD_OK)
            debugPrintf("[FMOD] SetPaused(true) failed: %s\n", FMOD_ErrorString(result));
    }
    channelData.playing = false;
}

static void removeChannel(HCHANNEL channel, unordered_map<HCHANNEL, ChannelData> &channels)
{
    auto channelIt = channels.find(channel);
    if (channelIt == channels.end())
        return;

    if (channelIt->second.channel != nullptr)
        FMOD_Channel_Stop(channelIt->second.channel);

    channels.erase(channelIt);
}

static void mainloop()
{
    unordered_map<HSAMPLE, SampleData> samples;
    unordered_map<HCHANNEL, ChannelData> channels;

    while (Work)
    {
        bool processedEvent = false;

        while (MyPool->head != MyPool->tail)
        {
            Event event = MyPool->events[MyPool->head];
            MemoryBarrier();
            MyPool->head = (MyPool->head + 1) % EVENT_POOL_SIZE;
            processedEvent = true;

            switch (event.type)
            {
            case EVENT_SAMPLE_LOADED:
                if (loadSampleData(event.data.sampleLoaded, samples))
                {
                    printf("[FMOD] Loaded sample %lu (%u bytes)\n", (unsigned long)event.data.sampleLoaded.sample, event.data.sampleLoaded.dataLength);
                    for (auto &entry : channels)
                    {
                        if (entry.second.sample != event.data.sampleLoaded.sample)
                            continue;

                        if (entry.second.playing)
                            ensurePlaying(entry.second, samples[event.data.sampleLoaded.sample]);
                        else if (entry.second.channel != nullptr)
                            ensurePaused(entry.second);
                    }
                }
                break;

            case EVENT_SAMPLE_FREED:
                removeSample(event.data.sampleFreed.sample, samples, channels);
                break;

            case EVENT_CHANNEL_CREATED:
            {
                ChannelData channelData;
                channelData.sample = event.data.channelCreated.sample;
                channelData.looping = (event.data.channelCreated.flags & BASS_SAMPLE_LOOP) != 0;
                channels[event.data.channelCreated.channel] = channelData;
                debugPrintf("[FMOD] Channel event created: sample=%lu channel=%lu flags=%u\n",
                            (unsigned long)event.data.channelCreated.sample,
                            (unsigned long)event.data.channelCreated.channel,
                            event.data.channelCreated.flags);
                break;
            }

            case EVENT_CHANNEL_UPDATED:
            {
                auto channelIt = channels.find(event.data.channelUpdated.channel);
                if (channelIt == channels.end())
                    break;

                ChannelData &channelData = channelIt->second;

                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_VOLUME)
                    channelData.volume = event.data.channelUpdated.volume;
                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_PAN)
                    channelData.pan = event.data.channelUpdated.pan;
                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_FREQUENCY)
                    channelData.frequency = event.data.channelUpdated.frequency;
                if (event.data.channelUpdated.updateMask & CHANNEL_UPDATE_LOOP)
                    channelData.looping = event.data.channelUpdated.looping != 0;

                applyChannelState(channelData);
                break;
            }

            case EVENT_CHANNEL_STATE:
            {
                auto channelIt = channels.find(event.data.channelState.channel);
                if (channelIt == channels.end())
                {
                    debugPrintf("[FMOD] Channel state ignored, missing channel %lu\n", (unsigned long)event.data.channelState.channel);
                    break;
                }

                channelIt->second.playing = event.data.channelState.state == CHANNEL_STATE_PLAYING;

                auto sampleIt = samples.find(channelIt->second.sample);
                if (sampleIt == samples.end())
                {
                    debugPrintf("[FMOD] Channel state deferred, missing sample %lu\n", (unsigned long)channelIt->second.sample);
                    break;
                }

                debugPrintf("[FMOD] Channel state: channel=%lu state=%u\n",
                            (unsigned long)event.data.channelState.channel,
                            event.data.channelState.state);

                if (channelIt->second.playing)
                    ensurePlaying(channelIt->second, sampleIt->second);
                else
                    ensurePaused(channelIt->second);

                break;
            }

            case EVENT_CHANNEL_REMOVED:
                removeChannel(event.data.channelRemoved.channel, channels);
                break;

            default:
                break;
            }

            FMOD_System_Update(fmodSystem);
        }

        if (!processedEvent)
            Sleep(1);
    }

    for (auto &entry : channels)
    {
        if (entry.second.channel != nullptr)
            FMOD_Channel_Stop(entry.second.channel);
    }

    for (auto &entry : samples)
    {
        if (entry.second.sound != nullptr)
            FMOD_Sound_Release(entry.second.sound);
    }
}

static bool UpPrivilege()
{
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;
    bool result = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken) != FALSE;
    if (!result)
        return false;
    result = LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tkp.Privileges[0].Luid) != FALSE;
    if (!result)
        return false;
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    return AdjustTokenPrivileges(hToken, FALSE, &tkp, sizeof(TOKEN_PRIVILEGES), NULL, NULL) != FALSE;
}

static HMODULE DllInject(HANDLE hProcess, const char *dllname)
{
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32 == 0)
        return 0;

    LPTHREAD_START_ROUTINE loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryA");
    if (loadLibrary == 0)
        return 0;

    SIZE_T dllNameLen = strlen(dllname) + 1;
    LPVOID remoteAddress = VirtualAllocEx(hProcess, NULL, dllNameLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteAddress == 0)
        return 0;

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, remoteAddress, (void *)dllname, dllNameLen, &bytesWritten))
    {
        VirtualFreeEx(hProcess, remoteAddress, 0, MEM_RELEASE);
        return 0;
    }

    HANDLE thread = CreateRemoteThread(hProcess, NULL, 0, loadLibrary, remoteAddress, 0, NULL);
    if (thread == 0)
    {
        VirtualFreeEx(hProcess, remoteAddress, 0, MEM_RELEASE);
        return 0;
    }

    WaitForSingleObject(thread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(hProcess, remoteAddress, 0, MEM_RELEASE);
    return (HMODULE)(uintptr_t)exitCode;
}

static DWORD getPID(LPCSTR ProcessName)
{
    HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(hProcessSnap, &pe32))
    {
        CloseHandle(hProcessSnap);
        return 0;
    }

    DWORD dwPid = 0;
    do
    {
        if (!strcmp(ProcessName, pe32.szExeFile))
        {
            dwPid = pe32.th32ProcessID;
            break;
        }
    } while (Process32Next(hProcessSnap, &pe32));

    CloseHandle(hProcessSnap);
    return dwPid;
}

static char osuExename[256] = "osu!.exe";

int main(int argc, char *argv[])
{
    init();
    printf("FMOD Studio Low Level API (C) Firelight Technologies Pty Ltd.\n");

#ifdef __EXESELECT
    printf("Input the osu executable name with extension name (exp: osu!.exe): ");
    char tmpc[256] = "";
    if (fgets(tmpc, sizeof(tmpc), stdin) != nullptr)
    {
        int length = (int)strlen(tmpc);
        while (length && (tmpc[length - 1] == ' ' || tmpc[length - 1] == '\n' || tmpc[length - 1] == '\r'))
            --length;
        tmpc[length] = '\0';
        if (length > 0)
            strcpy(osuExename, tmpc);
    }
    printf("Executable name is : \"%s\"\n", osuExename);
#endif

    FMOD_RESULT initRet = FMOD_System_Create(&fmodSystem, FMOD_VERSION);
    if (initRet != FMOD_OK)
    {
        printf("Create FMOD System Failed: %s\n", FMOD_ErrorString(initRet));
        system("pause");
        return 0;
    }

    HANDLE hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct sharepool), "ShareMemoryForOsuASIO4Play");
    LPVOID lpBase = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!(hMapFile && lpBase))
    {
        puts("ShareMemory Failed.");
        system("pause");
        return 0;
    }

    puts("ShareMemory Success.");
    MyPool = (struct sharepool *)lpBase;
    memset(MyPool, 0, sizeof(struct sharepool));
    printf("Pool Size: %dKB\n\n", (int)(sizeof(struct sharepool) / 1024));

    FMOD_System_SetDSPBufferSize(fmodSystem, bufferSize, 2);
    FMOD_System_SetOutput(fmodSystem, FMOD_OUTPUTTYPE_ASIO);
    FMOD_System_SetDriver(fmodSystem, driverId);

    FMOD_System_GetDriverInfo(fmodSystem, driverId, nullptr, 0, 0, &sampleRate, &driverSpeakerMode, &driverSpeakerChannels);
    if (driverSpeakerChannels <= 0)
        driverSpeakerChannels = 2;

    FMOD_System_SetSoftwareFormat(fmodSystem, sampleRate, driverSpeakerMode, driverSpeakerChannels);
    initRet = FMOD_System_Init(fmodSystem, 256, FMOD_INIT_NORMAL, 0);
    if (initRet != FMOD_OK)
    {
        printf("FMOD System Initialize Failed: %s\n", FMOD_ErrorString(initRet));
        system("pause");
        return 0;
    }

    Sleep(1000);

    unsigned bufLen;
    int bufNum;
    char name[256];
    int speakerChannels;
    FMOD_SPEAKERMODE speakerMode;
    FMOD_System_GetDSPBufferSize(fmodSystem, &bufLen, &bufNum);
    FMOD_System_GetDriverInfo(fmodSystem, driverId, name, 255, 0, 0, &speakerMode, &speakerChannels);
    printf("FMOD System Initialize Finished.\n");
    printf("[FMOD] Device Name: %s\n", name);
    printf("[FMOD] Device Sample Rate: %d\n", sampleRate);
    printf("[FMOD] Device Speaker Mode: %d\n", (int)speakerMode);
    printf("[FMOD] Device Channels: %d\n", speakerChannels);
    printf("[FMOD] DSP buffer size: %d * %d\n", bufLen, bufNum);
    printf("[FMOD] Latency: %.10lfms\n", bufLen * bufNum * 1000.0 / sampleRate);

    UpPrivilege();
    DWORD PID = 0;
    while (!PID)
    {
        PID = getPID(osuExename);
        Sleep(50);
    }
    Sleep(1000);

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, 0, PID);
    if (!hProcess)
    {
        puts("Opening Failed");
        system("pause");
        return 0;
    }

    char dllPath[512];
    strcpy(dllPath, _pgmptr);
    char *lastSlash = strrchr(dllPath, '\\');
    if (lastSlash != nullptr)
        lastSlash[1] = '\0';
    strcat(dllPath, "osu!asio_sound.dll");

    if (!DllInject(hProcess, dllPath))
    {
        puts("DLL injection failed");
        system("pause");
        return 0;
    }

    Work = true;
    HANDLE thread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)mainloop, 0, 0, 0);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
    CloseHandle(thread);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Work = false;

    if (fmodSystem != nullptr)
    {
        FMOD_System_Release(fmodSystem);
        printf("FMOD System released.\n");
    }

    UnmapViewOfFile(lpBase);
    CloseHandle(hMapFile);
    CloseHandle(hProcess);
    return 0;
}
