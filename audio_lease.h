#pragma once
#include <windows.h>
#include <cstring>
#include "sharepool.h"
#include "recovery.h"
#include "fmod/inc/fmod.h"

// A stalled event worker must not leave a looping ASIO voice audible after
// native failover. This pass-through DSP adds no queue, delay or allocation.
inline bool audioLeaseValid(const sharepool *pool, DWORD now)
{
    return pool && pool->magic == ASIO_PROTOCOL_MAGIC && pool->backendReady &&
        asio::heartbeatFresh(DWORD(pool->heartbeat),now);
}

inline FMOD_RESULT F_CALL readAudioLease(FMOD_DSP_STATE *state, float *input, float *output,
    unsigned length, int inputChannels, int *outputChannels)
{
    if (!output || !outputChannels || inputChannels < 1 || inputChannels > 32)
        return FMOD_ERR_DSP_FORMAT;
    *outputChannels = inputChannels;
    void *context = nullptr;
    bool active = state && state->functions && state->functions->getuserdata &&
        state->functions->getuserdata(state, &context) == FMOD_OK &&
        asio::recoveryLeaseValid(static_cast<sharepool *>(context), GetTickCount(),RECOVERY_SOURCE_DSP);
    size_t bytes = size_t(length) * inputChannels * sizeof(float);
    if (!active || !input) memset(output, 0, bytes);
    else if (output != input) memcpy(output, input, bytes);
    return FMOD_OK;
}

inline FMOD_RESULT attachAudioLease(FMOD_SYSTEM *system, sharepool *pool,
    FMOD_DSP **dsp, FMOD_CHANNELGROUP **master)
{
    FMOD_DSP_DESCRIPTION description = {};
    description.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
    strcpy_s(description.name, "ASIO ownership lease");
    description.version = 0x00010000;
    description.numinputbuffers = 1;
    description.numoutputbuffers = 1;
    description.read = readAudioLease;
    description.userdata = pool;
    FMOD_RESULT result = FMOD_System_GetMasterChannelGroup(system, master);
    if (result == FMOD_OK) result = FMOD_System_CreateDSP(system, &description, dsp);
    if (result == FMOD_OK) result = FMOD_ChannelGroup_AddDSP(*master, FMOD_CHANNELCONTROL_DSP_TAIL, *dsp);
    if (result != FMOD_OK && *dsp) { FMOD_DSP_Release(*dsp); *dsp = nullptr; }
    return result;
}
