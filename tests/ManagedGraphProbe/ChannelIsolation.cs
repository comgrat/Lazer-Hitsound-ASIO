using System.Reflection;
internal static class ChannelIsolation {
    const BindingFlags Flags=BindingFlags.Static|BindingFlags.NonPublic;
    static readonly List<(uint Channel,uint Generation,float Gain,uint Eligible)> calls=new();
    static int Ready()=>1;
    static uint Generation(uint channel)=>channel+100;
    static uint Playback(uint channel,uint allocation)=>allocation==Generation(channel)?allocation:0;
    static void Context(uint channel,uint generation,float gain,uint eligible)=>calls.Add((channel,generation,gain,eligible));
    static void Bind(string field,string method) {
        var target=typeof(Bridge).GetField(field,Flags)!;
        target.SetValue(null,typeof(ChannelIsolation).GetMethod(method,Flags)!.CreateDelegate(target.FieldType));
    }
    public static void Run(Action<bool,string> check) {
        Bind("ready",nameof(Ready));Bind("generation",nameof(Generation));Bind("playbackGeneration",nameof(Playback));Bind("context",nameof(Context));
        var bad=new Channel(10,new BadVolume());var good=new Channel(20,new GoodVolume());
        var exclusions=typeof(Bridge).GetField("exclusions",Flags)!.GetValue(null)!;
        var markerType=typeof(Bridge).GetNestedType("Marker",BindingFlags.NonPublic)!;
        var marker=Activator.CreateInstance(markerType,BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic,null,new object[]{true},null)!;
        exclusions.GetType().GetMethod("Add")!.Invoke(exclusions,new[]{good.AggregateVolume,marker});
        Bridge.ChannelAllocated(bad);Bridge.ChannelAllocated(good);
        Bridge.ChannelChanging(bad);
        check(calls.Any(c=>c.Channel==10&&c.Generation==110&&c.Eligible==0),"throwing channel sends only matching ineligible context");
        check(calls.All(c=>c.Channel!=0),"local graph failure never sends global shutdown context");
        Bridge.ChannelChanging(good);
        check(calls.Any(c=>c.Channel==20&&c.Generation==120&&c.Eligible==1&&c.Gain==1),"healthy gameplay channel remains eligible after another channel fault");
        check(!(bool)typeof(Bridge).GetField("failed",Flags)!.GetValue(null)!,"local rejection keeps bridge alive");
        // ObserveDrawableChain failure is also local and leaves original result.
        typeof(Bridge).GetField("liveMode",Flags)!.SetValue(null,true);
        bool original=false;Bridge.AllowZeroVolumeEvent(new Sound(),ref original);
        check(!original&&calls.All(c=>c.Channel!=0),"throwing drawable ancestor does not shut down session");
        typeof(Bridge).GetField("liveMode",Flags)!.SetValue(null,false);
        // Global installation/contract failure still uses the global disable path.
        typeof(Bridge).GetMethod("Disable",Flags)!.Invoke(null,new object[]{new InvalidOperationException("injected installation failure")});
        check(calls.Any(c=>c.Channel==0),"global installation failure retains global shutdown");
    }
    sealed class Mixer {public string Identifier="GameplayMixer";}
    sealed class Channel {public int channel;public object Mixer{get;}=new Mixer();public object AggregateVolume{get;}public Channel(int id,object volume){channel=id;AggregateVolume=volume;}}
    sealed class BadVolume {public double Value=>throw new InvalidOperationException("injected channel graph failure");}
    sealed class GoodVolume {public double Value=>0.5;}
    sealed class Sound {public object samplesContainer=new BadParent();}
    sealed class BadParent {public object Parent=>throw new InvalidOperationException("injected parent failure");}
}
