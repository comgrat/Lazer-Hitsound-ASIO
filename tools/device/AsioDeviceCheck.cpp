#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <cctype>
#include "../../fmod/inc/fmod.h"
#include "../../fmod/inc/fmod_errors.h"
#include "../../i18n.h"
namespace ui=asio::i18n;

struct SystemOwner
{
    FMOD_SYSTEM *value = nullptr;
    void release() { if (value) { FMOD_System_Close(value); FMOD_System_Release(value); value = nullptr; } }
    ~SystemOwner() { release(); }
};

static std::vector<float> makeTone(float frequency)
{
    const int rate = 48000, frames = 16800, fade = 480;
    std::vector<float> pcm(frames);
    for (int i = 0; i < frames; ++i)
    {
        float envelope = std::min(1.0f, std::min(float(i) / fade, float(frames - 1 - i) / fade));
        pcm[i] = 0.05f * envelope * std::sin(6.283185307179586f * frequency * i / rate);
    }
    return pcm;
}

static FMOD_RESULT playSide(FMOD_SYSTEM *system, bool left)
{
    auto pcm = makeTone(left ? 440.0f : 660.0f);
    FMOD_CREATESOUNDEXINFO info = {};
    info.cbsize = sizeof(info); info.length = unsigned(pcm.size() * sizeof(float));
    info.numchannels = 1; info.defaultfrequency = 48000; info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    FMOD_SOUND *sound = nullptr; FMOD_CHANNEL *channel = nullptr;
    FMOD_RESULT result = FMOD_System_CreateSound(system, reinterpret_cast<const char *>(pcm.data()),
        FMOD_OPENMEMORY | FMOD_OPENRAW | FMOD_CREATESAMPLE | FMOD_2D | FMOD_LOOP_OFF, &info, &sound);
    if (result == FMOD_OK) result = FMOD_System_PlaySound(system, sound, nullptr, true, &channel);
    float matrix[2] = { left ? 1.0f : 0.0f, left ? 0.0f : 1.0f };
    if (result == FMOD_OK) result = FMOD_Channel_SetMixMatrix(channel, matrix, 2, 1, 0);
    if (result == FMOD_OK) result = FMOD_Channel_SetPaused(channel, false);
    DWORD start = GetTickCount();
    while (result == FMOD_OK && DWORD(GetTickCount() - start) < 500)
    { result = FMOD_System_Update(system); Sleep(5); }
    if (channel) FMOD_Channel_Stop(channel);
    if (sound) FMOD_Sound_Release(sound);
    return result;
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    ui::initialize();
    int languageResult=ui::languageCommand(argc,argv);
    if(languageResult>=0)return languageResult;
    if(argc==1&&!ui::hasSavedLanguage()&&!ui::chooseLanguage())return 2;
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
    {
        auto pcm = makeTone(440);
        float peak = 0; double energy = 0;
        for (float value : pcm) { if (!std::isfinite(value)) return 1; peak = std::max(peak, std::abs(value)); energy += value * value; }
        bool ok = pcm.size() == 16800 && pcm.front() == 0 && pcm.back() == 0 && peak <= 0.050001f && peak > 0.04f && energy > 0;
        char peakText[40];snprintf(peakText,sizeof(peakText),"%.6f",peak);
        puts(ui::text("device.pcm",{ui::text(ok?"device.pass":"device.fail"),peakText}).c_str());
        if (!ok) return 1;
        SystemOwner backend;
        FMOD_RESULT check = FMOD_System_Create(&backend.value, FMOD_VERSION);
        if (check == FMOD_OK) check = FMOD_System_SetOutput(backend.value, FMOD_OUTPUTTYPE_NOSOUND);
        if (check == FMOD_OK) check = FMOD_System_SetSoftwareFormat(backend.value, 48000, FMOD_SPEAKERMODE_STEREO, 0);
        if (check == FMOD_OK) check = FMOD_System_Init(backend.value, 8, FMOD_INIT_NORMAL, nullptr);
        if (check == FMOD_OK) check = playSide(backend.value, true);
        if (check == FMOD_OK) check = playSide(backend.value, false);
        puts(ui::text("device.silent",{FMOD_ErrorString(check)}).c_str());
        return check == FMOD_OK ? 0 : 1;
    }
    bool listOnly = argc == 2 && strcmp(argv[1], "--list") == 0;
    if (argc != 1 && !listOnly) return 2;
    SystemOwner system;
    FMOD_RESULT result = FMOD_System_Create(&system.value, FMOD_VERSION);
    if (result == FMOD_OK) result = FMOD_System_SetOutput(system.value, FMOD_OUTPUTTYPE_ASIO);
    int count = 0;
    if (result == FMOD_OK) result = FMOD_System_GetNumDrivers(system.value, &count);
    if (result != FMOD_OK) { puts(ui::text("device.enumerate_failed",{FMOD_ErrorString(result)}).c_str()); return 1; }
    puts(ui::text("device.list").c_str());
    for (int id = 0; id < count; ++id)
    {
        char name[256] = {}; int rate = 0, channels = 0; FMOD_SPEAKERMODE mode;
        result = FMOD_System_GetDriverInfo(system.value, id, name, sizeof(name), nullptr, &rate, &mode, &channels);
        if (result != FMOD_OK) return 1;
        puts(ui::text("device.entry",{std::to_string(id),name,std::to_string(rate),std::to_string(channels)}).c_str());
    }
    if (listOnly) return 0;
    if (count < 1) { puts(ui::text("device.none").c_str()); return 3; }
    puts(ui::text("device.intro").c_str());
    printf("%s",ui::text("device.prompt").c_str()); fflush(stdout);
    char line[64]; if (!fgets(line, sizeof(line), stdin)) return 2;
    char *end = nullptr; long selected = strtol(line, &end, 10);
    bool hasNumber = end != line;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (!hasNumber || *end || selected < 0 || selected >= count) { puts(ui::text("device.invalid").c_str()); return 2; }
    puts(ui::text("device.confirm").c_str());
    fflush(stdout); if (!fgets(line, sizeof(line), stdin)) return 2;
    result = FMOD_System_SetDriver(system.value, int(selected));
    if (result == FMOD_OK) result = FMOD_System_SetDSPBufferSize(system.value, 16, 2);
    if (result == FMOD_OK) result = FMOD_System_SetSoftwareFormat(system.value, 48000, FMOD_SPEAKERMODE_STEREO, 0);
    if (result == FMOD_OK) result = FMOD_System_Init(system.value, 8, FMOD_INIT_NORMAL, nullptr);
    if (result != FMOD_OK) { puts(ui::text("device.open_failed",{FMOD_ErrorString(result)}).c_str()); return 1; }
    FMOD_ADVANCEDSETTINGS settings = {}; settings.cbSize = sizeof(settings);
    if (FMOD_System_GetAdvancedSettings(system.value, &settings) == FMOD_OK)
    {
        for (int i = 0; i < settings.ASIONumChannels && i < 64; ++i)
            puts(ui::text("device.channel",{std::to_string(i+1),
                settings.ASIOChannelList && settings.ASIOChannelList[i] ? settings.ASIOChannelList[i] : ui::text("device.unknown"),
                std::to_string(settings.ASIOSpeakerList ? int(settings.ASIOSpeakerList[i]) : -1)}).c_str());
    }
    puts(ui::text("device.left").c_str()); fflush(stdout);
    result = playSide(system.value, true);
    if (result == FMOD_OK) { puts(ui::text("device.right").c_str()); fflush(stdout); result = playSide(system.value, false); }
    system.release();
    if (result != FMOD_OK) { puts(ui::text("device.play_failed",{FMOD_ErrorString(result)}).c_str()); return 1; }
    puts(ui::text("device.done").c_str());
    return 0;
}
