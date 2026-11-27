// Exercise the production playback helpers against real FMOD's silent output.
// This opens no ASIO device and cannot establish acoustic correctness.
#define main AsioPlayerProgramMain
#include "../osu!asio_sound.cpp"
#undef main
#include <cmath>
#include <memory>

static int checks = 0, failures = 0;
static void check(bool condition, const char *name)
{
    ++checks;
    if (!condition) { ++failures; printf("FAIL: %s\n", name); }
}

static bool panFavours(FMOD_CHANNEL *voice, bool left)
{
    float matrix[64] = {};
    int outputs = 0, inputs = 0;
    if (FMOD_Channel_GetMixMatrix(voice, matrix, &outputs, &inputs, 8) != FMOD_OK ||
        outputs != 2 || inputs <= 0 || inputs > 8) return false;
    // FMOD can expose a stereo intermediate bus even for mono source PCM.
    float leftGain = 0, rightGain = 0;
    for (int i = 0; i < inputs; ++i)
    {
        leftGain += std::fabs(matrix[i]);
        rightGain += std::fabs(matrix[8 + i]);
    }
    return left ? leftGain > rightGain : rightGain > leftGain;
}

int main()
{
    check(FMOD_System_Create(&fmodSystem, FMOD_VERSION) == FMOD_OK, "create system");
    if (!fmodSystem) return 1;
    check(FMOD_System_SetOutput(fmodSystem, FMOD_OUTPUTTYPE_NOSOUND) == FMOD_OK, "silent output");
    check(FMOD_System_SetSoftwareFormat(fmodSystem, 48000, FMOD_SPEAKERMODE_STEREO, 0) == FMOD_OK, "stereo format");
    check(FMOD_System_Init(fmodSystem, 8, FMOD_INIT_NORMAL, nullptr) == FMOD_OK, "init");
    auto pool = std::make_unique<sharepool>();
    MyPool = pool.get();
    MyPool->backendReady = 1;
    outputVolume = 50;
    requestedOutputVolume = 50;
    requestedDebugLog = 0;
    debugLogEnabled = 0;
    check(FMOD_System_GetMasterChannelGroup(fmodSystem,&asioOutputGroup)==FMOD_OK &&
          FMOD_ChannelGroup_SetVolume(asioOutputGroup,0.5f)==FMOD_OK,"initial independent ASIO bus gain");

    // Load via the production mapping/ACK path, rather than bypassing it with
    // a pre-created FMOD sound. Also check stale generation rejection.
    SampleLoadedPayload payload = {};
    payload.sample = 17;
    payload.generation = 3;
    payload.sampleRate = 48000;
    payload.channels = 1;
    payload.format = SAMPLE_FORMAT_FLOAT;
    payload.dataLength = 48000 * sizeof(float);
    snprintf(payload.mappingName, sizeof(payload.mappingName), "Local\\AsioPlayerProbe_%lu", GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, payload.dataLength, payload.mappingName);
    float *pcm = static_cast<float *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, payload.dataLength));
    check(mapping && pcm, "PCM mapping");
    if (!pcm) { FMOD_System_Release(fmodSystem); return 1; }
    for (unsigned i = 0; i < 48000; ++i) pcm[i] = 0.01f * std::sin(float(i) * 0.0576f);
    auto &ack = MyPool->sampleAcks[payload.sample % SAMPLE_ACK_COUNT];
    ack.sample = payload.sample;
    ack.requestedGeneration = 2;
    unordered_map<HSAMPLE, SampleData> samples;
    check(!loadSampleData(payload, samples), "reject stale generation");
    ack.requestedGeneration = payload.generation;
    check(loadSampleData(payload, samples), "load mapped PCM");
    check(DWORD(ack.readyGeneration) == payload.generation, "ACK after load");

    ChannelData voice;
    voice.sample = payload.sample;
    voice.volume = 0.25f;
    voice.pan = -0.4f;
    voice.frequency = 24000;
    ensurePlaying(voice, samples.at(payload.sample));
    check(voice.channel != nullptr && voice.phase == ChannelData::Phase::Playing, "start production voice");
    float value = -1;
    float bus = -1;
    check(FMOD_Channel_GetVolume(voice.channel, &value) == FMOD_OK &&
          FMOD_ChannelGroup_GetVolume(asioOutputGroup,&bus)==FMOD_OK &&
          std::fabs(value * bus - 0.125f) < 0.00001f && value==0.25f,
          "sample gain and independent bus multiply once");
    auto voiceIdentity=voice.voiceId;
    ensurePlaying(voice,samples.at(payload.sample));
    check(voice.voiceId==voiceIdentity && playbackStats.starts==1,"same playing channel cannot create another voice");
    requestedOutputVolume=0;
    check(refreshOutputVolume() && FMOD_ChannelGroup_GetVolume(asioOutputGroup,&bus)==FMOD_OK && bus==0,"live 0 percent affects existing output");
    requestedOutputVolume=25;
    check(refreshOutputVolume() && FMOD_ChannelGroup_GetVolume(asioOutputGroup,&bus)==FMOD_OK && bus==0.25f,"live 25 percent bus gain");
    requestedOutputVolume=50;
    check(refreshOutputVolume(),"restore independent volume without restarting voice");
    FMOD_System_Update(fmodSystem);
    Sleep(40);
    check(panFavours(voice.channel, true), "negative pan favours left in stereo matrix");
    voice.pan = 0.4f;
    check(applyChannelState(voice) && panFavours(voice.channel, false), "positive pan favours right in stereo matrix");
    check(FMOD_Channel_GetFrequency(voice.channel, &value) == FMOD_OK && value == 24000, "preserve frequency");
    check(playbackStats.starts == 1, "count successful unpause");
    ensurePaused(voice);
    FMOD_BOOL paused = false;
    check(FMOD_Channel_GetPaused(voice.channel, &paused) == FMOD_OK && paused && voice.phase == ChannelData::Phase::Paused, "pause");
    ensurePlaying(voice, samples.at(payload.sample));
    check(playbackStats.starts == 2 && voice.phase == ChannelData::Phase::Playing && voice.voiceId==voiceIdentity,
          "resume same voice without duplicate creation");
    voice.volume = 0;
    check(applyChannelState(voice), "zero sample gain accepted");
    check(FMOD_Channel_GetVolume(voice.channel, &value) == FMOD_OK && value == 0, "zero sample gain stays silent");
    backendFailed(FMOD_ERR_OUTPUT_INIT);
    check(MyPool->backendReady == 0, "disable backend even with debug off");
    FMOD_CHANNELGROUP *master = nullptr;
    FMOD_BOOL muted = false;
    check(FMOD_System_GetMasterChannelGroup(fmodSystem, &master) == FMOD_OK &&
          FMOD_ChannelGroup_GetMute(master, &muted) == FMOD_OK && muted, "mute failed backend");
    mainThreadId = GetCurrentThreadId();
    MSG message = {};
    PeekMessage(&message, nullptr, 0, 0, PM_NOREMOVE);
    Work = true; backendFault = false;
    HANDLE worker = CreateThread(nullptr, 0, mainloop, nullptr, 0, nullptr);
    DWORD stopped = worker ? WaitForSingleObject(worker, 2000) : WAIT_FAILED;
    Work = false;
    if (worker) { WaitForSingleObject(worker, 2000); CloseHandle(worker); }
    check(stopped == WAIT_OBJECT_0 && backendFault, "failed queue/adapter stops the event worker within a bound");
    check(PeekMessage(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE,
          "failed backend notifies the owning loop to exit instead of silently remaining ready");
    FMOD_Channel_Stop(voice.channel);
    FMOD_Sound_Release(samples.at(payload.sample).sound);
    UnmapViewOfFile(pcm);
    CloseHandle(mapping);
    FMOD_System_Release(fmodSystem);
    fmodSystem = nullptr;
    MyPool = nullptr;
    printf("PlayerOutputProbe: %d checks, %d failures (FMOD NOSOUND; no acoustic validation)\n", checks, failures);
    return failures ? 1 : 0;
}
