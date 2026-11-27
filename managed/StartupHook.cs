using System.Collections;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using HarmonyLib;

// .NET startup-hook entry point. Opt-in only; never writes game assemblies or settings.
public static class StartupHook
{
    public static void Initialize()
    {
        if (Environment.GetEnvironmentVariable("ASIO_OFFLINE_ENABLE") != "1") return;
        UiText.Initialize();
        try { InstallEarly(); }
        catch (Exception exception) { Console.Error.WriteLine("[ASIO adapter] "+UiText.Get("managed.start_failed",exception.GetType().Name)); }
    }
    static void InstallEarly()
    {
        try { Console.OutputEncoding = new System.Text.UTF8Encoding(false); } catch { }
        string directory = Path.GetDirectoryName(typeof(StartupHook).Assembly.Location)!;
        AssemblyLoadContext.Default.Resolving += (_, name) =>
        {
            string candidate = Path.Combine(directory, name.Name + ".dll");
            try { return File.Exists(candidate) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(candidate) : null; }
            catch { return null; }
        };
        Bridge.Install();
    }
}

public static class Bridge
{
    const BindingFlags fields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    static readonly ConditionalWeakTable<object, VolumeNode> nodes = new();
    static readonly ConditionalWeakTable<object, Marker> exclusions = new();
    static readonly Harmony harmony = new("osu.asio.offline.volume.v2");
    static readonly object installationLock = new();
    static readonly HashSet<Assembly> installed = new();
    static MethodInfo? refreshAudioLayout;
    static Type? audioMixerType;
    public static AudioContract.ContractMode Mode { get; set; }=AudioContract.ContractMode.Adaptive;
    static bool contractValidated;
    static bool validatingContract;
    public static AudioContract.MatchStatus ContractStatus { get; private set; }=AudioContract.MatchStatus.Failed;
    static void ValidatePair(Assembly[] audio)
    {
        var result=AudioContract.Evaluate(audio.Single(a=>a.GetName().Name=="osu.Framework"),audio.Single(a=>a.GetName().Name=="osu.Game"),Mode);
        if(!result.Accepted){
            Log("[Contract Failed] "+UiText.Get("contract.failed"));
            foreach(string error in result.StructuralErrors)Log("[Contract Tier1] "+error);
            foreach(string error in result.SemanticErrors)Log("[Contract Tier2] "+error);
            foreach(string error in result.ExactDifferences)Log("[Contract Tier3] "+error);
            AudioContract.RequireAccepted(result);
        }
        Log((result.Status==AudioContract.MatchStatus.ExactMatch?"[Verified Contract] ":"[Compat/Adaptive] ")+UiText.Get(result.Status==AudioContract.MatchStatus.ExactMatch?"contract.verified":"contract.adaptive"));
        ContractStatus=result.Status;
        contractValidated=true;
    }
    sealed record VolumeNode(object Aggregate, IList Sources);
    sealed record Marker(bool Gameplay);
    sealed record ChannelToken(uint Handle, uint Generation);
    sealed class PlaybackIdentity { internal readonly long Id; internal long Calls; internal PlaybackIdentity(long id) { Id=id; } }
    static readonly ConditionalWeakTable<object, PlaybackIdentity> playbackIdentities = new();
    static long nextPlaybackIdentity;
    static readonly System.Collections.Concurrent.ConcurrentQueue<string> playbackTrace = new();
    static int playbackTraceCount;
    internal static bool detailedPlayback = Environment.GetEnvironmentVariable("ASIO_PLAYBACK_TRACE") == "1";
    static Timer? playbackTraceTimer;
    static PlaybackIdentity PlaybackId(object owner) => playbackIdentities.GetValue(owner, _ => new(Interlocked.Increment(ref nextPlaybackIdentity)));
    static void QueuePlaybackTrace(string message)
    {
        if (Interlocked.Increment(ref playbackTraceCount)>1024) { Interlocked.Decrement(ref playbackTraceCount); return; }
        playbackTrace.Enqueue(message);
    }
    static void FlushPlaybackTrace()
    {
        try { while (playbackTrace.TryDequeue(out var message)) { Interlocked.Decrement(ref playbackTraceCount); Log(message); } }
        catch { /* Logging failure must not alter game playback. */ }
    }
    public static void PlaybackRequested(object __instance)
    {
        if (!detailedPlayback) return;
        try {
            var id=PlaybackId(__instance);
            long call=Interlocked.Increment(ref id.Calls);
            QueuePlaybackTrace($"[ASIO playback trace] request tick_ms={Environment.TickCount64} object={id.Id} call={call} type={__instance.GetType().FullName}");
        } catch { /* The opt-in trace cannot affect the requested sound. */ }
    }
    static readonly ConditionalWeakTable<object, ChannelToken> tokens = new();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int ReadyDelegate();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void ContextDelegate(uint channel, uint generation, float gain, uint eligible);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate uint GenerationDelegate(uint channel);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate uint PlaybackGenerationDelegate(uint channel, uint allocation);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate uint TraceDelegate(uint index);
    static ReadyDelegate? ready;
    static ContextDelegate? context;
    static GenerationDelegate? generation;
    static PlaybackGenerationDelegate? playbackGeneration;
    static TraceDelegate? nativeTrace;
    static volatile bool failed;
    static volatile bool liveMode;
    public static TextWriter? LogWriter;
    public static void Log(string value) => (LogWriter ?? Console.Error).WriteLine(value);

    // Opt-in counters: no per-hit console/file writes on the audio thread.
    static class Trace
    {
        internal static readonly bool Enabled = Environment.GetEnvironmentVariable("ASIO_DIAGNOSTICS") == "1";
        internal static long Adjustments, MixerLoads, GameplayMarkers, Changing, Allocating, Tokens,
            MissingModule, MissingExports, NativeNotReady, MissingToken, ZeroChannel, ZeroGeneration,
            GameplayChannels, GraphMissing, Contexts, EligibleContexts, ZeroChecks, ZeroAllowed;
        internal static string LastMixer = "";
        static Timer? timer;
        internal static void Start()
        {
            if (Enabled) timer ??= new Timer(_ => Report(), null, 3000, 3000);
        }
        static long Read(ref long value) => Interlocked.Read(ref value);
        static void Report()
        {
            try
            {
                Log("[ASIO diagnostics] " + System.Text.Json.JsonSerializer.Serialize(new
                {
                    adjustments = Read(ref Adjustments), mixerLoads = Read(ref MixerLoads), gameplayMarkers = Read(ref GameplayMarkers),
                    changing = Read(ref Changing), allocating = Read(ref Allocating), tokens = Read(ref Tokens),
                    moduleMissing = Read(ref MissingModule), exportsMissing = Read(ref MissingExports), nativeNotReady = Read(ref NativeNotReady),
                    tokenMissing = Read(ref MissingToken), channelZero = Read(ref ZeroChannel), generationZero = Read(ref ZeroGeneration),
                    gameplayChannels = Read(ref GameplayChannels), graphMissing = Read(ref GraphMissing),
                    contexts = Read(ref Contexts), eligibleContexts = Read(ref EligibleContexts),
                    zeroChecks = Read(ref ZeroChecks), zeroAllowed = Read(ref ZeroAllowed),
                    lastMixer = Volatile.Read(ref LastMixer), failed,
                    native = nativeTrace is not { } read ? null : new
                    {
                        contexts = read(0), eligible = read(1), rejectedGeneration = read(2), mixerAdds = read(3),
                        startAttempts = read(4), untagged = read(5), backendUnavailable = read(6), sampleNotReady = read(7),
                        queueFull = read(8), nativeLocked = read(9), mixerUnpauses = read(10), publishedPlay = read(11)
                    }
                }));
            }
            catch { /* Diagnostics must never disable audio routing. */ }
        }
    }

    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern nint GetModuleHandleW(string name);

    public static void Install()
    {
        if (detailedPlayback) playbackTraceTimer ??= new Timer(_ => FlushPlaybackTrace(), null, 250, 250);
        Trace.Start();
        if (AppDomain.CurrentDomain.GetAssemblies().Any(a => a.GetName().Name is "osu.Framework" or "osu.Game"))
        { Disable(new InvalidOperationException(UiText.Get("managed.early_only"))); return; }
        Mode=ContractSettings.Read(Path.GetDirectoryName(Path.GetDirectoryName(typeof(Bridge).Assembly.Location))!);
        AppDomain.CurrentDomain.AssemblyLoad += (_, _) => {
            lock(installationLock){
                if(contractValidated||validatingContract||failed)return;
                var audio=AppDomain.CurrentDomain.GetAssemblies().Where(a=>a.GetName().Name is "osu.Framework" or "osu.Game").ToArray();
                if(audio.Length!=2)return;
                validatingContract=true;
                try{ValidatePair(audio);foreach(var assembly in audio)InstallAssembly(assembly);}catch(Exception error){Disable(error);}finally{validatingContract=false;}
            }
        };
    }

    public static bool InstallLive()
    {
        // Existing instances require lazy graph recovery, not merely removing
        // the early-start guard. Validate both current assemblies before patching.
        var deadline = Environment.TickCount64 + 10000;
        Assembly[] audio;
        do
        {
            audio = AppDomain.CurrentDomain.GetAssemblies().Where(a => a.GetName().Name is "osu.Framework" or "osu.Game").ToArray();
            if (audio.Length == 2) break;
            if (audio.Length > 2) throw new NotSupportedException(UiText.Get("managed.ambiguous"));
            // Only this bootstrap thread waits; game and audio threads continue.
            Thread.Sleep(25);
        } while (Environment.TickCount64 < deadline);
        if (audio.Length != 2) throw new NotSupportedException(UiText.Get("managed.timeout"));
        ValidatePair(audio);
        liveMode = true;
        if (detailedPlayback) playbackTraceTimer ??= new Timer(_ => FlushPlaybackTrace(), null, 250, 250);
        Trace.Start();
        foreach (var assembly in audio) InstallAssembly(assembly);
        Log("[ASIO adapter] "+UiText.Get(failed ? "managed.failed" : "managed.ready"));
        return !failed;
    }

    static void Patch(MethodBase method, string? prefix = null, string? postfix = null)
        => harmony.Patch(method, prefix is null ? null : new HarmonyMethod(typeof(Bridge), prefix),
            postfix is null ? null : new HarmonyMethod(typeof(Bridge), postfix));

    static void InstallAssembly(Assembly assembly)
    {
        string? name = assembly.GetName().Name;
        if (name is not ("osu.Framework" or "osu.Game")) return;
        lock (installationLock)
        {
            if (!installed.Add(assembly)) return;
            try
            {
                // Native-library/resource updates need not change audio code.
                // Accept reviewed implementation contracts; reject changed IL/layout.
                if(!contractValidated)throw new InvalidOperationException("Pair contract must be validated before any patches are installed.");
                if (name == "osu.Framework")
                {
                    var adjustments = assembly.GetType("osu.Framework.Audio.AudioAdjustments", true)!;
                    Patch(adjustments.GetConstructor(Type.EmptyTypes)!, postfix: nameof(AdjustmentsCreated));
                    var manager = assembly.GetType("osu.Framework.Audio.AudioManager", true)!;
                    foreach (var constructor in manager.GetConstructors(fields)) Patch(constructor, postfix: nameof(ManagerCreated));
                    var mixer = assembly.GetType("osu.Framework.Graphics.Audio.DrawableAudioMixer", true)!;
                    Patch(mixer.GetMethod("load", fields)!, postfix: nameof(MixerLoaded));
                    audioMixerType = assembly.GetType("osu.Framework.Audio.Mixing.IAudioMixer", true)!;
                    var wrapper = assembly.GetType("osu.Framework.Graphics.Audio.DrawableAudioWrapper", true)!;
                    refreshAudioLayout = wrapper.GetMethod("refreshLayoutFromParent", fields)
                        ?? throw new MissingMethodException(wrapper.FullName, "refreshLayoutFromParent");
                    var channel = assembly.GetType("osu.Framework.Audio.Sample.SampleChannelBass", true)!;
                    Patch(channel.GetMethod("OnStateChanged", fields)!, prefix: nameof(ChannelChanging));
                    if (detailedPlayback) Patch(channel.GetMethod("Play", fields)!, prefix: nameof(PlaybackRequested));
                    var allocate = channel.GetMethods(fields).Single(m => m.Name.StartsWith("<ensureChannel>b__", StringComparison.Ordinal));
                    Patch(allocate, postfix: nameof(ChannelAllocated));
                }
                else
                {
                    var pooledSample = assembly.GetType("osu.Game.Skinning.PoolableSkinnableSample", true)!;
                    harmony.Patch(pooledSample.GetMethod("Play", fields)!,
                        transpiler: new HarmonyMethod(typeof(Bridge), nameof(PrepareSampleBeforeGetChannel)));
                    var sound = assembly.GetType("osu.Game.Skinning.SkinnableSound", true)!;
                    Patch(sound.GetProperty("PlayWhenZeroVolume", fields)!.GetMethod!, postfix: nameof(AllowZeroVolumeEvent));
                    var playLambda = sound.GetMethods(fields).Single(m => m.Name.StartsWith("<Play>b__", StringComparison.Ordinal));
                    harmony.Patch(playLambda, transpiler: new HarmonyMethod(typeof(Bridge), nameof(ZeroVolumeGate)));
                }
                Log("[ASIO adapter] "+UiText.Get("managed.installed",name,assembly.GetName().Version));
            }
            catch (Exception exception) { Disable(exception); }
        }
    }

    static object? Field(object instance, string name) => AudioReflection.Field(instance, name);
    static object? Property(object instance, string name) => AudioReflection.Property(instance, name);
    static void Disable(Exception exception)
    {
        if (!failed) Log("[ASIO adapter] "+UiText.Get("managed.native",exception.GetType().Name,exception.Message));
        context?.Invoke(0, 0, 0, 0);
        failed = true;
    }

    sealed class LocalFault { }
    static readonly ConditionalWeakTable<object,LocalFault> localFaults=new();
    static void ReportLocalFault(object owner,Exception exception)
    {
        // Report once per object; diagnostic I/O failure cannot affect routing.
        if(owner is null || localFaults.TryGetValue(owner,out _))return;
        localFaults.GetValue(owner,static _=>new());
        try { Log("[ASIO adapter] "+UiText.Get("managed.channel_native",exception.GetType().Name)); } catch { }
    }
    static void ObserveAdjustments(object owner)
    {
        object aggregate=Field(owner,"volumeAggregate")??throw new InvalidOperationException("Missing volume aggregate");
        object result=Property(owner,"AggregateVolume")??throw new InvalidOperationException("Missing aggregate result");
        var sources=(IList)(Field(aggregate,"sourceMapping")??throw new InvalidOperationException("Missing aggregate sources"));
        nodes.GetValue(result,_=>new(aggregate,sources));
        if(Trace.Enabled)Interlocked.Increment(ref Trace.Adjustments);
    }
    public static void AdjustmentsCreated(object __instance)
    {
        try { ObserveAdjustments(__instance); }
        catch(Exception exception) { ReportLocalFault(__instance,exception); }
    }
    static void ObserveManager(object owner) => exclusions.GetValue(Property(owner,"Volume")??throw new InvalidOperationException("Missing manager volume"),_=>new(false));
    public static void ManagerCreated(object __instance)
    {
        try { ObserveManager(__instance); }
        catch (Exception exception) { Disable(exception); } // shared master setup
    }
    static bool IsGameplayMixer(object owner) =>
        (string?)(Field(owner, "Name") ?? Property(owner, "Name")) == "GameplayMixer";

    static void ObserveMixer(object owner)
    {
        if(Trace.Enabled)Interlocked.Increment(ref Trace.MixerLoads);
        if(IsGameplayMixer(owner)) {
            exclusions.GetValue(Property(owner,"Volume")??throw new InvalidOperationException("Missing mixer volume"),_=>new(true));
            if(Trace.Enabled)Interlocked.Increment(ref Trace.GameplayMarkers);
        }
    }
    public static void MixerLoaded(object __instance)
    {
        try { ObserveMixer(__instance); }
        catch(Exception exception) { Disable(exception); } // shared mixer setup
    }

    static bool IsType(object value, string fullName)
    {
        for (Type? type = value.GetType(); type != null; type = type.BaseType)
            if (type.FullName == fullName) return true;
        return false;
    }

    // Only inspect known audio owners and drawable ancestors. No heap scan,
    // client configuration, account data or writable volume setters are used.
    static void ObserveOwner(object owner)
    {
        object? adjustments = Field(owner, "adjustments");
        if (adjustments != null && IsType(adjustments, "osu.Framework.Audio.AudioAdjustments"))
            ObserveAdjustments(adjustments);
        if (IsType(owner, "osu.Framework.Audio.AudioManager")) ObserveManager(owner);
        if (IsType(owner, "osu.Framework.Graphics.Audio.DrawableAudioMixer")) ObserveMixer(owner);
    }
    static void ObserveDrawableChain(object start)
    {
        var visited = new HashSet<object>(ReferenceEqualityComparer.Instance);
        object? cursor = start;
        for (int depth = 0; cursor != null && depth < 128 && visited.Add(cursor); depth++)
        {
            ObserveOwner(cursor);
            object? component = Field(cursor, "component");
            if (component != null) ObserveOwner(component);
            cursor = Property(cursor, "Parent");
        }
    }

    static void RefreshGameplayLayout(object start)
    {
        var refresh = refreshAudioLayout;
        var mixerType = audioMixerType;
        if (refresh == null || mixerType == null) return;

        // Taiko replaces its pooled samples and plays them in the same update.
        // Resolve the game framework's pending parent bindings before GetChannel.
        List<object>? pending = null;
        object? cursor = start;
        for (int depth = 0; cursor != null && depth < 128; depth++, cursor = Property(cursor, "Parent"))
        {
            if (mixerType.IsInstanceOfType(cursor))
            {
                if (pending != null && IsType(cursor, "osu.Framework.Graphics.Audio.DrawableAudioMixer") && IsGameplayMixer(cursor))
                    for (int index = pending.Count - 1; index >= 0; index--)
                        refresh.Invoke(pending[index], null);
                return;
            }
            if (refresh.DeclaringType!.IsInstanceOfType(cursor) &&
                (Field(cursor, "fromParentLayout") is not { } layout || Property(layout, "IsValid") is not true))
                (pending ??= new()).Add(cursor);
        }
    }

    public static void PrepareGameplaySample(object sample)
    {
        try
        {
            if (liveMode && NativeReady()) RefreshGameplayLayout(sample);
        }
        catch (Exception exception) { ReportLocalFault(sample, exception); }
    }

    // The skin can create a new DrawableSample during Play, after the
    // zero-volume gate has already run. Prepare that exact instance.
    public static IEnumerable<CodeInstruction> PrepareSampleBeforeGetChannel(IEnumerable<CodeInstruction> instructions)
    {
        var result = new List<CodeInstruction>();
        int changed = 0;
        foreach (var instruction in instructions)
        {
            if (instruction.operand is MethodInfo method && method.Name == "GetChannel" &&
                method.DeclaringType?.FullName == "osu.Framework.Graphics.Audio.DrawableSample")
            {
                result.Add(new CodeInstruction(OpCodes.Dup).MoveLabelsFrom(instruction).MoveBlocksFrom(instruction));
                result.Add(new CodeInstruction(OpCodes.Call, typeof(Bridge).GetMethod(nameof(PrepareGameplaySample))));
                changed++;
            }
            result.Add(instruction);
        }
        if (changed != 1) throw new NotSupportedException("Expected one DrawableSample.GetChannel call in PoolableSkinnableSample.Play.");
        return result;
    }

    static void ObserveSoundGraph(object container)
    {
        ObserveDrawableChain(container);
        // A pre-existing SampleBass is bound to DrawableSample.AggregateVolume,
        // not directly to SkinnableSound's top-level container. Seed the child
        // wrappers too, otherwise an opaque zero product would hide chart gain.
        if (container is IEnumerable children)
            foreach (object child in children)
            {
                if (Field(child, "sampleContainer") is { } sampleContainer) ObserveDrawableChain(sampleContainer);
                if (Property(child, "Sample") is { } sample) ObserveDrawableChain(sample);
            }
    }
    static void RecoverAggregateOwner(object result)
    {
        if (nodes.TryGetValue(result, out _)) return;
        // AdjustableAudioComponent subscribes its own InvalidateState to its
        // aggregate result. This provides the original owner even if its
        // AudioAdjustments was constructed before this tool attached.
        if (Field(result, "ValueChanged") is not Delegate handlers) return;
        foreach (var handler in handlers.GetInvocationList())
            if (handler.Target is { } owner &&
                IsType(owner, "osu.Framework.Audio.AdjustableAudioComponent") &&
                ReferenceEquals(Property(owner, "AggregateVolume"), result)) ObserveOwner(owner);
    }

    sealed class GainScratch
    {
        public bool InUse;
        public readonly HashSet<object> Path = new(ReferenceEqualityComparer.Instance);
        public readonly List<object>?[] Sources = new List<object>?[65];
        public void Clear()
        {
            Path.Clear();
            foreach (var sources in Sources) sources?.Clear();
        }
    }
    [ThreadStatic] static GainScratch? gainScratch;

    // Walk original sources, not bound-copy results divided by a user gain.
    public static bool TrySampleGain(object volume, out double gain, out bool gameplay)
    {
        gameplay = false;
        var scratch = gainScratch ??= new GainScratch();
        // A getter can re-enter the adapter on the same thread. Keep the outer
        // traversal intact; only unusual nested calls allocate another buffer.
        if (scratch.InUse) scratch = new GainScratch();
        scratch.InUse = true;
        try
        {
            bool found = false;
            gain = Evaluate(volume, scratch, ref found, 0);
            gameplay = found;
            return !failed && double.IsFinite(gain) && gain >= 0 && gain <= 1;
        }
        catch (Exception exception) { gain = 0; ReportLocalFault(volume,exception); return false; }
        finally { scratch.Clear(); scratch.InUse = false; }
    }
    static double Evaluate(object volume, GainScratch scratch, ref bool gameplay, int depth)
    {
        if (depth > 64 || !scratch.Path.Add(volume)) throw new InvalidOperationException(UiText.Get("managed.graph"));
        try
        {
            if (exclusions.TryGetValue(volume, out var marker)) { gameplay |= marker.Gameplay; return 1; }
            if (liveMode) RecoverAggregateOwner(volume);
            if (!nodes.TryGetValue(volume, out var node)) return Convert.ToDouble(Property(volume, "Value"));
            var sources = scratch.Sources[depth] ??= new List<object>();
            sources.Clear();
            try
            {
                lock (node.Sources)
                {
                    foreach (object pair in node.Sources)
                    {
                        object weak = Property(pair, "WeakReference")!;
                        if (AudioReflection.WeakTarget(weak) is { } target) sources.Add(target);
                    }
                }
                double result = Convert.ToDouble(Field(node.Aggregate, "initialValue"));
                foreach (object source in sources) result *= Evaluate(source, scratch, ref gameplay, depth + 1);
                return result;
            }
            finally { sources.Clear(); }
        }
        finally { scratch.Path.Remove(volume); }
    }
    static bool NativeReady()
    {
        if (failed) return false;
        if (ready is null)
        {
            nint module = GetModuleHandleW("osu!asio_sound.dll");
            if (module == 0)
            {
                if (Trace.Enabled) Interlocked.Increment(ref Trace.MissingModule);
                return false;
            }
            if (!NativeLibrary.TryGetExport(module, "AsioBridgeReady", out nint readyAddress) ||
                !NativeLibrary.TryGetExport(module, "AsioChannelContextV2", out nint contextAddress) ||
                !NativeLibrary.TryGetExport(module, "AsioChannelGeneration", out nint generationAddress))
            {
                if (Trace.Enabled) Interlocked.Increment(ref Trace.MissingExports);
                return false;
            }
            context = Marshal.GetDelegateForFunctionPointer<ContextDelegate>(contextAddress);
            generation = Marshal.GetDelegateForFunctionPointer<GenerationDelegate>(generationAddress);
            if (NativeLibrary.TryGetExport(module,"AsioChannelPlaybackGeneration",out nint playbackAddress))
                playbackGeneration=Marshal.GetDelegateForFunctionPointer<PlaybackGenerationDelegate>(playbackAddress);
            if (Trace.Enabled && NativeLibrary.TryGetExport(module, "AsioBridgeTrace", out nint traceAddress))
                nativeTrace = Marshal.GetDelegateForFunctionPointer<TraceDelegate>(traceAddress);
            Volatile.Write(ref ready, Marshal.GetDelegateForFunctionPointer<ReadyDelegate>(readyAddress));
        }
        bool result = ready() != 0;
        if (!result && Trace.Enabled) Interlocked.Increment(ref Trace.NativeNotReady);
        return result;
    }
    static void RejectChannel(object owner,uint channel,Exception exception)
    {
        ReportLocalFault(owner,exception);
        try {
            if(channel==0 && tokens.TryGetValue(owner,out var remembered))channel=remembered.Handle;
            if(channel==0 || context is null)return;
            if(!tokens.TryGetValue(owner,out var token) || token.Handle!=channel)return;
            uint current=playbackGeneration is null?token.Generation:playbackGeneration(channel,token.Generation);
            if(current!=0)context(channel,current,0,0);
        }catch { /* A bad object's callback must never invoke global disable. */ }
    }
    public static void ChannelChanging(object __instance)
    {
        uint channel=0;
        try
        {
            if (Trace.Enabled) Interlocked.Increment(ref Trace.Changing);
            if (!NativeReady()) return;
            channel = unchecked((uint)(int)Field(__instance, "channel")!);
            if (channel == 0) { if (Trace.Enabled) Interlocked.Increment(ref Trace.ZeroChannel); return; }
            if (!tokens.TryGetValue(__instance, out var token) || token.Handle != channel)
            {
                // Handles created after native hooks but before managed attach
                // have a native generation even though allocation was missed.
                uint current = liveMode ? generation!(channel) : 0;
                if (current == 0) { if (Trace.Enabled) Interlocked.Increment(ref Trace.MissingToken); return; }
                token = new(channel, current);
                tokens.AddOrUpdate(__instance, token);
            }
            object? mixer = Property(__instance, "Mixer");
            string? identifier = mixer == null ? null : (string?)Field(mixer, "Identifier");
            bool eligible = identifier == "GameplayMixer";
            if (Trace.Enabled)
            {
                Volatile.Write(ref Trace.LastMixer, identifier ?? "<null>");
                if (eligible) Interlocked.Increment(ref Trace.GameplayChannels);
            }
            double gain = 0;
            if (eligible)
            {
                if (liveMode)
                {
                    ObserveOwner(__instance);
                    if (Field(mixer!, "manager") is { } manager) ObserveOwner(manager);
                }
                bool valid = TrySampleGain(Property(__instance, "AggregateVolume")!, out gain, out bool gameplay);
                if (Trace.Enabled && (!valid || !gameplay)) Interlocked.Increment(ref Trace.GraphMissing);
                eligible = valid && gameplay;
            }
            uint playbackToken = playbackGeneration is null ? token.Generation : playbackGeneration(channel,token.Generation);
            if(playbackToken==0) return; // Old object may not claim a reused native handle.
            context!(channel, playbackToken, (float)gain, eligible ? 1u : 0u);
            if (Trace.Enabled)
            {
                Interlocked.Increment(ref Trace.Contexts);
                if (eligible) Interlocked.Increment(ref Trace.EligibleContexts);
            }
        }
        catch (Exception exception) { RejectChannel(__instance,channel,exception); }
    }
    public static void ChannelAllocated(object __instance)
    {
        uint channel=0;
        try
        {
            if (Trace.Enabled) Interlocked.Increment(ref Trace.Allocating);
            if (!NativeReady()) return;
            channel = unchecked((uint)(int)Field(__instance, "channel")!);
            uint token = generation!(channel);
            if (channel == 0 || token == 0) { if (Trace.Enabled) Interlocked.Increment(ref Trace.ZeroGeneration); return; }
            tokens.AddOrUpdate(__instance,new(channel, token));
            if (detailedPlayback) {
                var id=PlaybackId(__instance);
                QueuePlaybackTrace($"[ASIO playback trace] channel tick_ms={Environment.TickCount64} object={id.Id} call={Interlocked.Read(ref id.Calls)} channel={channel} generation={token}");
            }
            if (Trace.Enabled) Interlocked.Increment(ref Trace.Tokens);
        }
        catch (Exception exception) { RejectChannel(__instance,channel,exception); }
    }
    public static void AllowZeroVolumeEvent(object __instance, ref bool __result)
    {
        try
        {
            if (Trace.Enabled) Interlocked.Increment(ref Trace.ZeroChecks);
            object container = Field(__instance, "samplesContainer")!;
            bool nativeReady = NativeReady();
            // Seed even when Looping already makes the original getter true.
            // Its subsequent channel callback still needs all wrapper nodes.
            if (liveMode)
            {
                if (nativeReady) RefreshGameplayLayout(container);
                ObserveSoundGraph(container);
            }
            if (__result || !nativeReady) return;
            if (TrySampleGain(Property(container, "AggregateVolume")!, out _, out bool gameplay) && gameplay)
            {
                __result = true;
                if (Trace.Enabled) Interlocked.Increment(ref Trace.ZeroAllowed);
            }
        }
        catch (Exception exception) { ReportLocalFault(__instance,exception); }
    }

    // Patch the actual Play lambda call site as well as its tiny getter, so a
    // future JIT inline of the original getter cannot bypass the zero-volume gate.
    public static IEnumerable<CodeInstruction> ZeroVolumeGate(IEnumerable<CodeInstruction> instructions)
    {
        var result = instructions.ToList();
        int changed = 0;
        foreach (var instruction in result)
            if (instruction.operand is MethodInfo method && method.Name == "get_PlayWhenZeroVolume")
            {
                instruction.opcode = OpCodes.Call;
                instruction.operand = typeof(Bridge).GetMethod(nameof(ShouldPlayAtZero));
                changed++;
            }
        if (changed != 1) throw new NotSupportedException(UiText.Get("managed.il"));
        return result;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static bool ShouldPlayAtZero(object sound)
    {
        try {
            bool result = (bool)Property(sound, "Looping")!;
            AllowZeroVolumeEvent(sound, ref result);
            return result;
        }catch(Exception exception){ReportLocalFault(sound,exception);return false;}
    }
}
