// Independent, device-free checks for the external-output lease DSP.
// This does not replace the real-FMOD silent probe; it pins the callback's
// format/pointer contract and its fail-closed behavior when the pool lease is
// unhealthy. No game, audio device or timing claim is involved.
#include <array>
#include <cstdio>
#include "../audio_lease.h"

static int checks;
static int failures;

static void check(bool condition, const char *label)
{
    ++checks;
    if (condition) printf("PASS %s\n", label);
    else { ++failures; printf("FAIL %s\n", label); }
}

static FMOD_RESULT F_CALL userData(FMOD_DSP_STATE *state, void **data)
{
    *data = state->plugindata;
    return FMOD_OK;
}

static FMOD_RESULT F_CALL userDataFailure(FMOD_DSP_STATE *, void **)
{
    return FMOD_ERR_INVALID_HANDLE;
}

static bool allEqual(const float *buffer, size_t count, float value)
{
    for (size_t i = 0; i < count; ++i)
        if (buffer[i] != value) return false;
    return true;
}

int main()
{
    static sharepool pool = {};
    pool.magic = ASIO_PROTOCOL_MAGIC;
    pool.backendReady = 1;
    pool.heartbeat = LONG(GetTickCount());

    FMOD_DSP_STATE_FUNCTIONS functions = {};
    functions.getuserdata = userData;
    FMOD_DSP_STATE state = {};
    state.functions = &functions;
    state.plugindata = &pool;

    std::array<float, 32 * 16> input, output;
    input.fill(0.25f);

    // Healthy pass-through must preserve every supported channel width and set
    // outchannels to the input width. 1/2/6/32 covers mono through the FMOD
    // maximum, including the 6-channel device case exercised by the main probe.
    for (int channels : { 1, 2, 6, 32 })
    {
        output.fill(-1.0f);
        int outChannels = 0;
        FMOD_RESULT result = readAudioLease(&state, input.data(), output.data(), 16, channels, &outChannels);
        char label[96];
        snprintf(label, sizeof(label), "healthy %d-channel block copied and outchannels set", channels);
        check(result == FMOD_OK && outChannels == channels &&
              allEqual(output.data(), size_t(16) * channels, 0.25f), label);
    }

    // A zero backendReady must silence even when magic and heartbeat are fresh.
    pool.backendReady = 0;
    output.fill(-1.0f);
    int outChannels = 0;
    check(readAudioLease(&state, input.data(), output.data(), 16, 6, &outChannels) == FMOD_OK,
          "ready=0 callback still succeeds");
    check(outChannels == 6 && allEqual(output.data(), 16 * 6, 0.0f),
          "ready=0 zeroes output and reports input channels");
    pool.backendReady = 1;

    // Heartbeat boundary is deterministic through audioLeaseValid: 249 ms is
    // healthy, exactly 250 ms is expired. The wrap case proves DWORD subtraction.
    pool.heartbeat = 0xfffffff0;
    check(audioLeaseValid(&pool, 0xfffffff0u), "heartbeat age 0 is valid");
    check(audioLeaseValid(&pool, 0xfffffff0u + 249u), "heartbeat age 249 ms is valid");
    check(!audioLeaseValid(&pool, 0xfffffff0u + 250u), "heartbeat age 250 ms is stale");
    check(audioLeaseValid(&pool, 5u), "wrapped heartbeat age is valid (21 ms)");

    // Stale heartbeat, bad magic, null state, missing functions, and failed
    // getuserdata all fail closed with silence instead of passing old audio.
    pool.heartbeat = LONG(GetTickCount() - 300);
    output.fill(-1.0f);
    check(readAudioLease(&state, input.data(), output.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(output.data(), 16 * 6, 0.0f), "stale heartbeat zeroes output");

    pool.heartbeat = LONG(GetTickCount());
    check(pool.recoveryState==RECOVERY_REQUESTED,"expiry latches a recovery request");
    output.fill(-1.0f);
    check(readAudioLease(&state,input.data(),output.data(),16,6,&outChannels)==FMOD_OK &&
          allEqual(output.data(),16*6,0.0f),"heartbeat refresh cannot revive abandoned output");
    // Model a completed handshake for the remaining format tests.
    pool.recoveryState=RECOVERY_RUNNING;
    pool.magic = 0;
    output.fill(-1.0f);
    check(readAudioLease(&state, input.data(), output.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(output.data(), 16 * 6, 0.0f), "wrong magic zeroes output");
    pool.magic = ASIO_PROTOCOL_MAGIC;

    output.fill(-1.0f);
    check(readAudioLease(nullptr, input.data(), output.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(output.data(), 16 * 6, 0.0f), "null DSP state zeroes output");
    FMOD_DSP_STATE noFunctions = {};
    output.fill(-1.0f);
    check(readAudioLease(&noFunctions, input.data(), output.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(output.data(), 16 * 6, 0.0f), "missing DSP functions zeroes output");
    functions.getuserdata = userDataFailure;
    output.fill(-1.0f);
    check(readAudioLease(&state, input.data(), output.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(output.data(), 16 * 6, 0.0f), "failed getuserdata zeroes output");
    functions.getuserdata = userData;

    // Pointer/format errors return FMOD_ERR_DSP_FORMAT and must not write.
    check(readAudioLease(&state, input.data(), nullptr, 16, 6, &outChannels) == FMOD_ERR_DSP_FORMAT,
          "null output is rejected");
    check(readAudioLease(&state, input.data(), output.data(), 16, 6, nullptr) == FMOD_ERR_DSP_FORMAT,
          "null outputChannels is rejected");
    check(readAudioLease(&state, input.data(), output.data(), 16, 0, &outChannels) == FMOD_ERR_DSP_FORMAT,
          "zero input channels is rejected");
    check(readAudioLease(&state, input.data(), output.data(), 16, 33, &outChannels) == FMOD_ERR_DSP_FORMAT,
          "33 input channels is rejected");

    // Null input with a healthy lease is silence, not a crash.
    output.fill(-1.0f);
    check(readAudioLease(&state, nullptr, output.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(output.data(), 16 * 6, 0.0f), "null input zeroes output");

    // In-place active blocks are left untouched; in-place inactive blocks are
    // zeroed. Both are required because FMOD may hand back the same buffer.
    input.fill(0.25f);
    check(readAudioLease(&state, input.data(), input.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(input.data(), 16 * 6, 0.25f), "in-place active block is preserved");
    pool.backendReady = 0;
    check(readAudioLease(&state, input.data(), input.data(), 16, 6, &outChannels) == FMOD_OK &&
          allEqual(input.data(), 16 * 6, 0.0f), "in-place inactive block is zeroed");
    pool.backendReady = 1;

    // Canary check: the callback must write exactly length*channels floats.
    std::array<float, 16 * 6 + 8> guarded;
    guarded.fill(123.0f);
    input.fill(0.5f);
    check(readAudioLease(&state, input.data(), guarded.data() + 2, 16, 6, &outChannels) == FMOD_OK &&
          guarded[0] == 123.0f && guarded[1] == 123.0f &&
          allEqual(guarded.data() + 2, 16 * 6, 0.5f) &&
          guarded[2 + 16 * 6] == 123.0f && guarded[2 + 16 * 6 + 5] == 123.0f,
          "writes exactly length*channels floats (canaries intact)");

    printf("audio lease mock: checks=%d failures=%d; no device or timing claim\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
