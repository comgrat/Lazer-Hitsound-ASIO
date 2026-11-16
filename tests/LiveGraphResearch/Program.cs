using System.Collections;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Text.Json;

namespace LiveGraphResearch;

internal static class Program
{
    const BindingFlags All = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    const BindingFlags StaticAll = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
    const BindingFlags Any = All | BindingFlags.Static;

    static Assembly framework = null!;
    static Assembly game = null!;
    static object volumeKind = null!;
    static int checks;
    static int failures;
    static string client = "";
    static readonly List<CheckResult> results = new();

    static int Main(string[] args)
    {
        string? jsonPath = null;
        try
        {
            if(args.Length==0) throw new ArgumentException("Pass the path to a compatible client directory; only its audio assemblies are loaded.");
            client = Path.GetFullPath(args[0]);
            if (args.Length > 1) jsonPath = Path.GetFullPath(args[1]);
            Console.WriteLine($"client={client}");
            Console.WriteLine($"runtime={Environment.Version}");

            LoadClient(client);
            Console.WriteLine($"framework={framework.GetName().Version} MVID={framework.ManifestModule.ModuleVersionId}");
            Console.WriteLine($"game={game.GetName().Version} MVID={game.ManifestModule.ModuleVersionId}");
            volumeKind = Enum.Parse(framework.GetType("osu.Framework.Audio.AdjustableProperty", true)!, "Volume");

            // Real objects, real graph, constructed BEFORE any late attach. The
            // production early hooks therefore cannot have observed them; only
            // InstallLive's on-demand recovery can make these eligible.
            Fixture chart = BuildFixture("GameplayMixer");
            Fixture earlyReturn = BuildFixture("GameplayMixer");
            Fixture ui = BuildFixture("UIMixer");
            Console.WriteLine("three-layer fixtures built before InstallLive");
            Check(!ReferenceEquals(GetF(chart.Top, "adjustments"), GetF(chart.DrawableSample, "adjustments")),
                "fixture has distinct top/sample-container/drawable adjustments");
            Check(SourceCount(chart.SampleBass) == 3,
                "sample aggregate is own+factory+DrawableSample (not top container shortcut)");
            Check(SourceCount(chart.DrawableSample) == 2,
                "DrawableSample aggregate is own+sampleContainer parent");

            Type soundType = T("osu.Game.Skinning.SkinnableSound", game);
            MethodInfo originalGetter = soundType.GetProperty("PlayWhenZeroVolume", All)!.GetMethod!;
            MethodInfo originalLambda = soundType.GetMethods(All).Single(m => m.Name.StartsWith("<Play>b__", StringComparison.Ordinal));
            bool getterPreJit = Prepare(originalGetter, out string getterJitError);
            bool lambdaPreJit = Prepare(originalLambda, out string lambdaJitError);
            Check(getterPreJit, "original getter JIT-prepared before late attach", getterJitError);
            Check(lambdaPreJit, "original <Play> lambda JIT-prepared before late attach", lambdaJitError);

            bool installed = Bridge.InstallLive();
            Check(installed, "Bridge.InstallLive returns true");
            if (!installed) throw new InvalidOperationException("InstallLive failed; refusing to pretend graph checks ran");

            InstallNativeStubs();
            CheckPatches(originalGetter, originalLambda);

            // The getter is already JITted. A direct invocation goes through the
            // patched entry point; this is not evidence about an inlined copy in
            // the original Play lambda (which we deliberately do not invoke).
            SetUserVolume(chart, 0.0);
            SetP(chart.Sound, "Looping", false);
            bool patchedGetter = (bool)originalGetter.Invoke(chart.Sound, null)!;
            Check(patchedGetter, "pre-JITted getter postfix allows gameplay zero on direct invocation");

            // 0/1/25/50/100% user volume. Both the master volume and the
            // GameplayMixer volume are user-controlled and must be excluded;
            // the two non-excluded 0.5 drawable levels must survive as 0.25.
            foreach (double percent in new[] { 0.0, 0.01, 0.25, 0.5, 1.0 })
            {
                SetUserVolume(chart, percent);
                bool allow = false;
                Bridge.AllowZeroVolumeEvent(chart.Sound, ref allow);
                Check(allow, $"zero-volume event allowed at user={percent:P0}");

                Send(chart, out uint eligible, out float gain);
                Check(eligible == 1, $"channel eligible at user={percent:P0} (identifier GameplayMixer, wrapper graph reachable)");
                Check(Math.Abs(gain - 0.25f) < 1e-5f,
                    $"chart gain 0.5*0.5=0.25 at user={percent:P0} (actual {gain:F6})");
            }

            // G2: the getter can return true (Looping) and the zero gate returns
            // early. The wrapper graph must still be seeded for a later channel.
            SetP(earlyReturn.Sound, "Looping", true);
            bool earlyResult = Bridge.ShouldPlayAtZero(earlyReturn.Sound);
            Check(earlyResult, "Looping zero-gate result remains true");
            SetP(earlyReturn.Sound, "Looping", false);
            Send(earlyReturn, out uint earlyEligible, out float earlyGain);
            Check(earlyEligible == 1, "channel after zero-getter early return still eligible (G2 seed-before-return)");
            Check(Math.Abs(earlyGain - 0.25f) < 1e-5f,
                $"channel after early return still has chart gain 0.25 (actual {earlyGain:F6})");

            // UI mixer must not become gameplay, and must not be marked by the
            // zero-volume gate or by the channel context.
            bool uiAllow = Bridge.ShouldPlayAtZero(ui.Sound);
            Check(!uiAllow, "UI sound is not allowed by the zero-volume gate");
            Send(ui, out uint uiEligible, out _);
            Check(uiEligible == 0, "UI channel is not eligible (UIMixer is not GameplayMixer)");
            // An unrelated UI owner must never have its volume graph inspected.
            // Its graph getter throws, so this also catches accidental recovery.
            var uiWithoutGraph = new UiWithoutGraph(17, GetP(ui.Channel, "Mixer")!);
            Bridge.ChannelAllocated(uiWithoutGraph);
            int beforeUi = Stub.Contexts;
            Bridge.ChannelChanging(uiWithoutGraph);
            Check(Stub.Contexts == beforeUi + 1 && Stub.Eligible == 0 && Stub.Gain == 0,
                "non-gameplay context bypasses a throwing volume graph");

            // Chart-original zero must stay silent while the graph is still valid.
            SetUserVolume(chart, 1.0);
            SetVolume(chart.SampleContainer, 0.0);
            Send(chart, out uint zeroEligible, out float zeroGain);
            Check(zeroEligible == 1, "chart-original zero remains graph-eligible");
            Check(zeroGain == 0.0f, $"chart-original zero stays silent (actual {zeroGain:F6})");
            SetVolume(chart.SampleContainer, 0.5);

            // Native unavailable: no allow, no context, no exception. Existing
            // routing state must not be mutated by a failed readiness check.
            Stub.Alive = false;
            int contextsBefore = Stub.Contexts;
            bool offlineAllow = Bridge.ShouldPlayAtZero(chart.Sound);
            Check(!offlineAllow, "native unavailable does not force the zero-volume event");
            Bridge.ChannelChanging(chart.Channel);
            Check(Stub.Contexts == contextsBefore, "native unavailable sends no channel context");
            Stub.Alive = true;

            // The replacement path is the code the Play transpiler calls. This
            // is exercised; the original pre-JIT Play lambda itself is not.
            SetUserVolume(chart, 0.0);
            Check(Bridge.TrySampleGain(GetP(chart.Channel, "AggregateVolume")!, out double directGain, out bool gameplay)
                  && gameplay && Math.Abs(directGain - 0.25) < 1e-9,
                $"direct TrySampleGain returns gameplay 0.25 (actual {directGain:F6}, gameplay={gameplay})");
        }
        catch (Exception exception)
        {
            failures++;
            results.Add(new CheckResult("fixture/install", false, exception.GetType().Name + ": " + exception.Message));
            Console.Error.WriteLine("EXCEPTION " + exception);
        }
        finally
        {
            try { WriteResult(jsonPath); }
            catch (Exception exception) { Console.Error.WriteLine("RESULT WRITE FAILED " + exception); }
        }

        Console.WriteLine($"checks={checks} failures={failures}");
        return failures == 0 ? 0 : 1;
    }

    static void LoadClient(string directory)
    {
        if (!Directory.Exists(directory)) throw new DirectoryNotFoundException(directory);
        AssemblyLoadContext.Default.Resolving += (context, name) =>
        {
            if (string.IsNullOrEmpty(name.Name)) return null;
            string candidate = Path.Combine(directory, name.Name + ".dll");
            return File.Exists(candidate) ? context.LoadFromAssemblyPath(candidate) : null;
        };
        framework = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(directory, "osu.Framework.dll"));
        game = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(directory, "osu.Game.dll"));
    }

    static Type T(string name, Assembly assembly) => assembly.GetType(name, true)!;

    static FieldInfo FindField(Type? type, string name)
    {
        for (; type != null; type = type.BaseType)
        {
            FieldInfo? field = type.GetField(name, All | BindingFlags.DeclaredOnly);
            if (field != null) return field;
        }
        throw new MissingFieldException(name);
    }

    static PropertyInfo FindProperty(Type? type, string name)
    {
        for (; type != null; type = type.BaseType)
        {
            PropertyInfo? property = type.GetProperty(name, All | BindingFlags.DeclaredOnly);
            if (property != null) return property;
        }
        throw new MissingMemberException(name);
    }

    static object? GetF(object instance, string name) => FindField(instance.GetType(), name).GetValue(instance);
    static void SetF(object instance, string name, object? value) => FindField(instance.GetType(), name).SetValue(instance, value);
    static object? GetP(object instance, string name) => FindProperty(instance.GetType(), name).GetValue(instance);
    static void SetP(object instance, string name, object? value) => FindProperty(instance.GetType(), name).SetValue(instance, value);

    static object? Call(object instance, string name, params object?[] arguments)
    {
        for (Type? type = instance.GetType(); type != null; type = type.BaseType)
        {
            foreach (MethodInfo method in type.GetMethods(All | BindingFlags.DeclaredOnly))
            {
                if (method.Name != name || method.GetParameters().Length != arguments.Length) continue;
                ParameterInfo[] parameters = method.GetParameters();
                bool compatible = true;
                for (int i = 0; i < parameters.Length; i++)
                {
                    if (arguments[i] == null) continue;
                    if (!parameters[i].ParameterType.IsInstanceOfType(arguments[i])) { compatible = false; break; }
                }
                if (compatible) return method.Invoke(instance, arguments);
            }
        }
        throw new MissingMethodException(instance.GetType().FullName + "::" + name);
    }

    static int SourceCount(object owner)
    {
        object adjustments;
        try { adjustments = GetP(owner, "Adjustments")!; }
        catch { adjustments = GetF(owner, "adjustments")!; }
        object aggregate = GetF(adjustments, "volumeAggregate")!;
        return ((IList)GetF(aggregate, "sourceMapping")!).Count;
    }

    static void Dump(string label, object owner)
    {
        object adjustments;
        try { adjustments = GetP(owner, "Adjustments")!; }
        catch { adjustments = GetF(owner, "adjustments")!; }
        object aggregate = GetF(adjustments, "volumeAggregate")!;
        var sources = (IList)GetF(aggregate, "sourceMapping")!;
        object result = GetP(adjustments, "AggregateVolume")!;
        object? parent = null;
        try { parent = GetP(owner, "Parent"); } catch { }
        Console.WriteLine($"DUMP {label}: value={GetP(result, "Value")} sources={sources.Count} adjustments={adjustments.GetType().Name} parent={parent?.GetType().Name ?? "<null>"}");
    }

    static void Refresh(object wrapper)
    {
        for (Type? type = wrapper.GetType(); type != null; type = type.BaseType)
        {
            MethodInfo? method = type.GetMethod("refreshLayoutFromParent", All | BindingFlags.DeclaredOnly);
            if (method != null) { method.Invoke(wrapper, null); return; }
        }
        throw new MissingMethodException(wrapper.GetType().FullName + "::refreshLayoutFromParent");
    }

    static object NewDouble(double value) =>
        Activator.CreateInstance(framework.GetType("osu.Framework.Bindables.BindableDouble", true)!, value)!;

    static object NewInt(int value) =>
        Activator.CreateInstance(framework.GetType("osu.Framework.Bindables.BindableInt", true)!, value)!;

    static void SetVolume(object owner, double value)
    {
        object bindable = GetP(owner, "Volume")!;
        SetP(bindable, "Value", value);
    }

    static void SetUserVolume(Fixture fixture, double value)
    {
        SetVolume(fixture.Mixer, value);
        SetVolume(fixture.Manager, value);
    }

    sealed class Fixture
    {
        public object Sound = null!;
        public object Top = null!;
        public object Pool = null!;
        public object SampleContainer = null!;
        public object DrawableSample = null!;
        public object SampleBass = null!;
        public object Factory = null!;
        public object Store = null!;
        public object Manager = null!;
        public object Mixer = null!;
        public object Channel = null!;
    }

    // Real osu.Framework/osu.Game types; only fields that would otherwise be
    // populated by the game host are set through reflection. No game code runs,
    // no BASS call is made, no ASIO device is opened.
    static Fixture BuildFixture(string mixerName)
    {
        var fixture = new Fixture();

        object manager = RuntimeHelpers.GetUninitializedObject(T("osu.Framework.Audio.AudioManager", framework));
        SetF(manager, "VolumeSample", NewDouble(1));
        _ = GetP(manager, "Adjustments"); // create/subscribe the real AudioAdjustments
        fixture.Manager = manager;

        object store = RuntimeHelpers.GetUninitializedObject(T("osu.Framework.Audio.Sample.SampleStore", framework));
        _ = GetP(store, "Adjustments");
        fixture.Store = store;

        object factory = RuntimeHelpers.GetUninitializedObject(T("osu.Framework.Audio.Sample.SampleBassFactory", framework));
        _ = GetP(factory, "Adjustments");
        fixture.Factory = factory;

        object sample = RuntimeHelpers.GetUninitializedObject(T("osu.Framework.Audio.Sample.SampleBass", framework));
        SetF(sample, "<Name>k__BackingField", "fixture-sample");
        SetF(sample, "<PlaybackConcurrency>k__BackingField", NewInt(2));
        SetF(sample, "factory", factory);
        _ = GetP(sample, "Adjustments");
        fixture.SampleBass = sample;

        object drawableSample = Activator.CreateInstance(
            T("osu.Framework.Graphics.Audio.DrawableSample", framework), sample, true)!;
        fixture.DrawableSample = drawableSample;

        object pool = Activator.CreateInstance(T("osu.Game.Skinning.PoolableSkinnableSample", game))!;
        SetP(pool, "Sample", drawableSample);
        object poolContainer = GetF(pool, "sampleContainer")!;
        Call(poolContainer, "Add", drawableSample);
        fixture.Pool = pool;
        fixture.SampleContainer = poolContainer;

        object sound = Activator.CreateInstance(T("osu.Game.Skinning.SkinnableSound", game))!;
        object top = GetF(sound, "samplesContainer")!;
        Call(top, "Add", pool);
        fixture.Sound = sound;
        fixture.Top = top;

        object mixer = Activator.CreateInstance(T("osu.Framework.Graphics.Audio.DrawableAudioMixer", framework))!;
        SetF(mixer, "Name", mixerName);
        _ = GetF(mixer, "adjustments"); // DrawableAudioWrapper field, not an AdjustableAudioComponent property
        Call(mixer, "Add", sound);
        fixture.Mixer = mixer;

        // Real BindAdjustments/AddAdjustment calls, mirroring AddItem:
        // channel <- sample <- factory <- store <- manager; wrapper <- container.
        Call(store, "BindAdjustments", manager);
        Call(store, "AddAdjustment", volumeKind, GetF(manager, "VolumeSample"));
        Call(factory, "BindAdjustments", store);
        Call(sample, "BindAdjustments", factory);

        object channel = RuntimeHelpers.GetUninitializedObject(T("osu.Framework.Audio.Sample.SampleChannelBass", framework));
        SetF(channel, "channel", 123);
        SetF(channel, "sample", sample);
        _ = GetP(channel, "Adjustments");
        Call(channel, "BindAdjustments", sample);

        object bassMixer = RuntimeHelpers.GetUninitializedObject(T("osu.Framework.Audio.Mixing.Bass.BassAudioMixer", framework));
        SetF(bassMixer, "manager", manager);
        SetF(bassMixer, "Identifier", mixerName);
        SetP(channel, "Mixer", bassMixer);
        fixture.Channel = channel;

        // A host would assign Parent during loading. This harness is not a game
        // host, so assign the real Parent property (internal setter) explicitly;
        // otherwise refreshLayoutFromParent cannot see the real hierarchy.
        SetP(top, "Parent", sound);
        SetP(sound, "Parent", mixer);
        SetP(pool, "Parent", top);
        SetP(poolContainer, "Parent", pool);
        SetP(drawableSample, "Parent", poolContainer);

        // Refresh the real parent chain once, exactly like Drawable.Update would.
        Refresh(drawableSample);
        Refresh(poolContainer);
        Refresh(top);

        SetVolume(top, 0.5);
        SetVolume(poolContainer, 0.5);
        SetVolume(drawableSample, 1.0);
        SetVolume(sample, 1.0);
        SetVolume(factory, 1.0);
        SetVolume(store, 1.0);
        SetVolume(channel, 1.0);
        SetVolume(mixer, 0.0);
        SetVolume(manager, 0.0);
        Dump("channel", channel);
        Dump("sample", sample);
        Dump("factory", factory);
        Dump("store", store);
        Dump("manager", manager);
        Dump("mixer", mixer);
        Dump("top", top);
        Dump("pool", poolContainer);
        Dump("drawable", drawableSample);
        return fixture;
    }

    static readonly object sync = new();

    static void Check(bool condition, string name, string? detail = null)
    {
        lock (sync)
        {
            checks++;
            if (!condition) failures++;
            results.Add(new CheckResult(name, condition, detail));
            Console.WriteLine((condition ? "PASS " : "FAIL ") + name + (detail == null ? "" : " | " + detail));
        }
    }

    static void Send(Fixture fixture, out uint eligible, out float gain)
    {
        int before = Stub.Contexts;
        Bridge.ChannelAllocated(fixture.Channel);
        Bridge.ChannelChanging(fixture.Channel);
        eligible = Stub.Eligible;
        gain = Stub.Gain;
        Check(Stub.Contexts == before + 1, "context emitted");
    }

    static void InstallNativeStubs()
    {
        SetDelegate("ready", nameof(Stub.Ready));
        SetDelegate("context", nameof(Stub.Context));
        SetDelegate("generation", nameof(Stub.Generation));
    }

    static void SetDelegate(string fieldName, string methodName)
    {
        FieldInfo field = typeof(Bridge).GetField(fieldName, StaticAll)
            ?? throw new MissingFieldException(typeof(Bridge).FullName, fieldName);
        MethodInfo method = typeof(Stub).GetMethod(methodName, StaticAll)
            ?? throw new MissingMethodException(typeof(Stub).FullName, methodName);
        field.SetValue(null, Delegate.CreateDelegate(field.FieldType, method));
    }

    static bool Prepare(MethodBase method, out string error)
    {
        try
        {
            RuntimeHelpers.PrepareMethod(method.MethodHandle);
            error = "";
            return true;
        }
        catch (Exception exception)
        {
            error = exception.GetType().Name + ": " + exception.Message;
            return false;
        }
    }

    static void CheckPatches(MethodInfo getter, MethodInfo lambda)
    {
        Check(HasPatch(getter), "Harmony patch recorded on PlayWhenZeroVolume getter");
        Check(HasPatch(lambda), "Harmony patch recorded on <Play> lambda");
    }

    static bool HasPatch(MethodBase method)
    {
        object? harmony = typeof(Bridge).GetField("harmony", StaticAll)?.GetValue(null);
        if (harmony == null) return false;
        Type type = harmony.GetType();
        MethodInfo? getPatchInfo = type.GetMethod("GetPatchInfo", Any, null, new[] { typeof(MethodBase) }, null);
        if (getPatchInfo == null) return false;
        object? info = getPatchInfo.Invoke(getPatchInfo.IsStatic ? null : harmony, new object[] { method });
        if (info == null) return false;
        IEnumerable? owners = info.GetType().GetProperty("Owners", All)?.GetValue(info) as IEnumerable;
        return owners != null && owners.Cast<object>().Any(owner => owner?.ToString() == "osu.asio.offline.volume.v2");
    }

    static void WriteResult(string? jsonPath)
    {
        var payload = new
        {
            task = "P2",
            client,
            runtime = Environment.Version.ToString(),
            checks,
            failures,
            passed = failures == 0,
            results,
        };
        string json = JsonSerializer.Serialize(payload, new JsonSerializerOptions { WriteIndented = true });
        Console.WriteLine("RESULT_JSON_BEGIN");
        Console.WriteLine(json);
        Console.WriteLine("RESULT_JSON_END");
        if (jsonPath != null)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(jsonPath)!);
            File.WriteAllText(jsonPath, json);
        }
    }

    sealed record CheckResult(string Name, bool Pass, string? Detail);

    sealed class UiWithoutGraph
    {
        public int channel;
        public object Mixer { get; }
        public object AggregateVolume => throw new InvalidOperationException("UI graph was evaluated");
        public UiWithoutGraph(int handle, object mixer) { channel=handle; Mixer=mixer; }
    }

    static class Stub
    {
        public static bool Alive = true;
        public static int Contexts;
        public static uint Eligible;
        public static float Gain;
        public static int Ready() => Alive ? 1 : 0;
        public static uint Generation(uint handle) => 9;
        public static void Context(uint handle, uint generation, float gain, uint eligible)
        {
            Contexts++;
            Gain = gain;
            Eligible = eligible;
        }
    }
}
