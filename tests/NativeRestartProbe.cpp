#define main NativeHookMockProgramMain
#include "NativeHookMock.cpp"
#undef main

int main(){
    InitializeCriticalSection(&queueLock);InitializeCriticalSection(&stateLock);InitializeCriticalSection(&logLock);
    locksInitialised=true;logLockInitialised=true;debugLogEnabled=0;
    g_pool=new sharepool;MyPool=g_pool;
    resetFixture();
    Fixture voice=createChannel(true,0.25f);
    setGameVolume(voice.channel,0.8f);
    clearEvents();
    CHECK_TRUE(MyBASS_ChannelPlay(voice.channel,FALSE));
    DWORD first=channelGeneration(voice.channel);
    DWORD allocation=channelInfoMap[voice.channel].incarnation;
    auto events=eventsSnapshot();
    CHECK_EQ(countState(events,CHANNEL_STATE_PLAYING),1);
    clearEvents();g_mock.active=1;
    CHECK_TRUE(MyBASS_ChannelPlay(voice.channel,FALSE));
    events=eventsSnapshot();
    CHECK_EQ(countState(events,CHANNEL_STATE_PLAYING),0);
    CHECK_EQ(channelGeneration(voice.channel),first);
    clearEvents();
    CHECK_TRUE(MyBASS_ChannelPlay(voice.channel,TRUE));
    DWORD restarted=channelGeneration(voice.channel);
    events=eventsSnapshot();
    CHECK_TRUE(restarted!=first);
    CHECK_EQ(countRemoved(events,first),1);
    CHECK_EQ(countCreated(events,restarted),1);
    CHECK_EQ(countState(events,CHANNEL_STATE_PLAYING),1);
    CHECK_NEAR(g_mock.nativeVolume[voice.channel],0,0);
    CHECK_EQ(AsioChannelPlaybackGeneration(voice.channel,allocation),restarted);
    AsioChannelContextV2(voice.channel,AsioChannelPlaybackGeneration(voice.channel,allocation),0.7f,1);
    CHECK_NEAR(channelInfoMap[voice.channel].sampleGain,0.7f,0.000001f);
    CHECK_EQ(AsioChannelPlaybackGeneration(voice.channel,allocation+9999),0);
    clearEvents();g_mock.active=0;
    CHECK_TRUE(MyBASS_ChannelPlay(voice.channel,FALSE));
    DWORD endedReplay=channelGeneration(voice.channel);
    events=eventsSnapshot();
    CHECK_TRUE(endedReplay!=restarted);
    CHECK_EQ(countRemoved(events,restarted),1);
    CHECK_EQ(countState(events,CHANNEL_STATE_PLAYING),1);
    clearEvents();g_mock.playOk=false;
    CHECK_FALSE(MyBASS_ChannelPlay(voice.channel,TRUE));
    CHECK_EQ(channelGeneration(voice.channel),endedReplay);
    events=eventsSnapshot();CHECK_EQ(events.size(),0);
    g_mock.playOk=true;g_mock.active=1;
    clearEvents();
    CHECK_TRUE(MyBASS_ChannelPause(voice.channel));
    CHECK_TRUE(MyBASS_ChannelPause(voice.channel));
    CHECK_TRUE(MyBASS_Mixer_ChannelRemove(voice.channel));
    events=eventsSnapshot();
    CHECK_EQ(countState(events,CHANNEL_STATE_PAUSED),1);
    HCHANNEL reused=voice.channel;
    CHECK_TRUE(MyBASS_StreamFree(reused));
    g_mock.channelHandle=reused;
    HCHANNEL replacement=MyBASS_SampleGetChannel(voice.sample,0);
    CHECK_EQ(replacement,reused);
    CHECK_EQ(AsioChannelPlaybackGeneration(reused,allocation),0);
    printf("NativeRestartProbe: checks=%d failures=%d; real hook + mock BASS, no acoustic validation\n",g_checks,g_failures);
    return g_failures?1:0;
}
