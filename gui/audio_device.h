#pragma once
// ASIO device helpers shared by the window and its command-line checks. Only
// enumeration is safe while the backend owns the device; opening is not.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "../fmod/inc/fmod.h"
#include "../fmod/inc/fmod_errors.h"
#include "../i18n.h"

namespace lazer {

struct AsioDevice {
    int id = 0;
    std::string name;
    int rate = 0, channels = 0;
};

struct FmodSystem {
    FMOD_SYSTEM *value = nullptr;
    void release() { if (value) { FMOD_System_Release(value); value = nullptr; } }
    ~FmodSystem() { release(); }
};

// Lists drivers without initializing any of them.
inline FMOD_RESULT enumerateDevices(std::vector<AsioDevice> &devices)
{
    devices.clear();
    FmodSystem system;
    FMOD_RESULT result = FMOD_System_Create(&system.value, FMOD_VERSION);
    if (result != FMOD_OK) return result;
    result = FMOD_System_SetOutput(system.value, FMOD_OUTPUTTYPE_ASIO);
    int count = 0;
    if (result == FMOD_OK) result = FMOD_System_GetNumDrivers(system.value, &count);
    if (result != FMOD_OK) return result;
    for (int i = 0; i < count; ++i) {
        char name[256] = {};
        AsioDevice device;
        device.id = i;
        if (FMOD_System_GetDriverInfo(system.value, i, name, sizeof(name), nullptr, &device.rate, nullptr, &device.channels) == FMOD_OK) {
            device.name = name;
            devices.push_back(device);
        }
    }
    return FMOD_OK;
}

// Opens the selected device exactly as the backend will, then releases it.
// A sample rate of 0 resolves to the driver default and is returned in rate.
inline FMOD_RESULT openDevice(FmodSystem &system, int driver, int buffer, int &rate)
{
    FMOD_RESULT result = FMOD_System_Create(&system.value, FMOD_VERSION);
    if (result == FMOD_OK) result = FMOD_System_SetOutput(system.value, FMOD_OUTPUTTYPE_ASIO);
    if (result == FMOD_OK) result = FMOD_System_SetDriver(system.value, driver);
    if (result == FMOD_OK && !rate) {
        result = FMOD_System_GetDriverInfo(system.value, driver, nullptr, 0, nullptr, &rate, nullptr, nullptr);
        if (result == FMOD_OK && rate < 8000) result = FMOD_ERR_OUTPUT_FORMAT;
    }
    if (result == FMOD_OK) result = FMOD_System_SetDSPBufferSize(system.value, unsigned(buffer), 2);
    if (result == FMOD_OK) result = FMOD_System_SetSoftwareFormat(system.value, rate, FMOD_SPEAKERMODE_STEREO, 0);
    if (result == FMOD_OK) result = FMOD_System_Init(system.value, 32, FMOD_INIT_NORMAL, nullptr);
    return result;
}

// Returns the resolved rate in rate, so a saved 0 becomes the actual default.
inline FMOD_RESULT checkDevice(int driver, int buffer, int &rate)
{
    FmodSystem system;
    return openDevice(system, driver, buffer, rate);
}

inline std::vector<float> makeTone(float frequency, int rate)
{
    const int frames = rate * 35 / 100, fade = rate / 100;
    std::vector<float> pcm(size_t(std::max(frames, 1)));
    for (int i = 0; i < frames; ++i) {
        float envelope = std::min(1.0f, std::min(float(i) / fade, float(frames - 1 - i) / fade));
        pcm[size_t(i)] = 0.05f * envelope * std::sin(6.283185307179586f * frequency * i / rate);
    }
    return pcm;
}

// One quiet tone on Playback 1 (left) or Playback 2 (right).
inline FMOD_RESULT playSide(FMOD_SYSTEM *system, bool left, int rate)
{
    auto pcm = makeTone(left ? 440.0f : 660.0f, rate);
    FMOD_CREATESOUNDEXINFO info = {};
    info.cbsize = sizeof(info);
    info.length = unsigned(pcm.size() * sizeof(float));
    info.numchannels = 1;
    info.defaultfrequency = rate;
    info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    FMOD_SOUND *sound = nullptr;
    FMOD_CHANNEL *channel = nullptr;
    FMOD_RESULT result = FMOD_System_CreateSound(system, reinterpret_cast<const char *>(pcm.data()),
        FMOD_OPENMEMORY | FMOD_OPENRAW | FMOD_CREATESAMPLE | FMOD_2D | FMOD_LOOP_OFF, &info, &sound);
    if (result == FMOD_OK) result = FMOD_System_PlaySound(system, sound, nullptr, true, &channel);
    float matrix[2] = { left ? 1.0f : 0.0f, left ? 0.0f : 1.0f };
    if (result == FMOD_OK) result = FMOD_Channel_SetMixMatrix(channel, matrix, 2, 1, 0);
    if (result == FMOD_OK) result = FMOD_Channel_SetPaused(channel, false);
    const DWORD start = GetTickCount();
    while (result == FMOD_OK && DWORD(GetTickCount() - start) < 500) { result = FMOD_System_Update(system); Sleep(5); }
    if (channel) FMOD_Channel_Stop(channel);
    if (sound) FMOD_Sound_Release(sound);
    return result;
}

inline FMOD_RESULT playTestTone(int driver, int buffer, int rate, bool left)
{
    FmodSystem system;
    FMOD_RESULT result = openDevice(system, driver, buffer, rate);
    if (result == FMOD_OK) result = playSide(system.value, left, rate);
    if (system.value) FMOD_System_Close(system.value);
    return result;
}

inline std::string fmodError(FMOD_RESULT result) { return FMOD_ErrorString(result); }

// Command-line enumeration used by package checks: never opens a device, and
// an empty driver list is a successful result.
inline int listDevicesCommand()
{
    namespace ui = asio::i18n;
    FmodSystem system;
    FMOD_RESULT result = FMOD_System_Create(&system.value, FMOD_VERSION);
    if (result != FMOD_OK) { puts(ui::text("config.create_failed", {FMOD_ErrorString(result)}).c_str()); return 1; }
    result = FMOD_System_SetOutput(system.value, FMOD_OUTPUTTYPE_ASIO);
    int count = 0;
    if (result == FMOD_OK) result = FMOD_System_GetNumDrivers(system.value, &count);
    if (result != FMOD_OK) { puts(ui::text("config.enumerate_failed", {FMOD_ErrorString(result)}).c_str()); return 1; }
    puts(ui::text("config.devices").c_str());
    for (int i = 0; i < count; ++i) {
        char name[256] = {};
        int rate = 0, channels = 0;
        if (FMOD_System_GetDriverInfo(system.value, i, name, sizeof(name), nullptr, &rate, nullptr, &channels) == FMOD_OK)
            puts(ui::text("config.device_line", {std::to_string(i), name, std::to_string(rate), std::to_string(channels)}).c_str());
    }
    if (!count) puts(ui::text("config.no_devices").c_str());
    return 0;
}

} // namespace lazer
