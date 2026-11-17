using System.Reflection;
using System.Reflection.Emit;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

public static partial class AudioContract
{
    public enum ContractMode { Strict = 0, Adaptive = 1 }
    public enum MatchStatus { Failed, ExactMatch, AdaptiveMatch }
    public sealed record MemberRule(string Id, string Assembly, string Type, string Kind, string Name, string[] Signatures);
    public sealed record AnchorRule(string Id, string Assembly, string Type, string Method, string[] Instructions, string? Signature = null);
    public sealed record PolicyProfile(string Id, MemberRule[] Structure, AnchorRule[] Anchors, int ProjectionRevision = 1);
    public sealed record ReviewedContract(string Id, string Policy, Snapshot Framework, Snapshot Game);
    public sealed record Catalog(int SchemaVersion, PolicyProfile[] Profiles, ReviewedContract[] ReviewedContracts);
    public sealed record CheckResult(MatchStatus Status, string[] StructuralErrors, string[] SemanticErrors, string[] ExactDifferences, string? Policy, string? ReviewedId)
    {
        public bool Accepted => Status != MatchStatus.Failed;
    }
    public sealed record Candidate(string Kind, DateTime ExtractedUtc, PolicyProfile ObservedPolicy, Snapshot Framework, Snapshot Game, CheckResult Validation);
    public static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true, Converters = { new JsonStringEnumConverter() } };
    static readonly Lazy<Catalog> embedded = new(() => {
        using Stream resource = typeof(AudioContract).Assembly.GetManifestResourceStream("Asio.AudioContract.json") ?? throw new InvalidOperationException(UiText.Get("contract.missing"));
        return ReadCatalog(resource);
    });
    public static Catalog EmbeddedCatalog => embedded.Value;
    public static Catalog ReadCatalog(Stream stream)
    {
        var catalog=JsonSerializer.Deserialize<Catalog>(stream,JsonOptions) ?? throw new InvalidDataException("Empty audio contract catalog.");
        if(catalog.SchemaVersion!=2 || catalog.Profiles is not { Length: >0 } || catalog.ReviewedContracts is null ||
           catalog.Profiles.Any(p=>p==null||p.ProjectionRevision is not (1 or 2)||string.IsNullOrWhiteSpace(p.Id)||p.Structure is not {Length: >0}||p.Anchors is not {Length: >0}||
               p.Structure.Any(r=>r==null||string.IsNullOrWhiteSpace(r.Id)||string.IsNullOrWhiteSpace(r.Assembly)||string.IsNullOrWhiteSpace(r.Type)||
                   r.Kind is not ("type" or "field" or "property" or "method" or "generated" or "constructor")||r.Signatures is not {Length: >0}||r.Signatures.Any(string.IsNullOrEmpty))||
               p.Anchors.Any(r=>r==null||string.IsNullOrWhiteSpace(r.Id)||string.IsNullOrWhiteSpace(r.Type)||string.IsNullOrWhiteSpace(r.Method)||r.Instructions is not {Length: >0}||r.Instructions.Any(string.IsNullOrEmpty))||
               p.Structure.Select(r=>r.Id).Distinct(StringComparer.Ordinal).Count()!=p.Structure.Length||
               p.Anchors.Select(r=>r.Id).Distinct(StringComparer.Ordinal).Count()!=p.Anchors.Length) ||
           catalog.Profiles.Select(p=>p.Id).Distinct(StringComparer.Ordinal).Count()!=catalog.Profiles.Length ||
           catalog.ReviewedContracts.Any(r=>r==null||string.IsNullOrWhiteSpace(r.Id)||!catalog.Profiles.Any(p=>p.Id==r.Policy)||
               r.Framework?.Assembly!="osu.Framework"||r.Game?.Assembly!="osu.Game"||r.Framework.Types is not {Count: >0}||r.Game.Types is not {Count: >0}||
               r.Framework.Types.Concat(r.Game.Types).Any(p=>string.IsNullOrWhiteSpace(p.Key)||p.Value==null||p.Value.Length!=64||p.Value.Any(c=>!Uri.IsHexDigit(c))))||
           catalog.ReviewedContracts.Select(r=>r.Id).Distinct(StringComparer.Ordinal).Count()!=catalog.ReviewedContracts.Length)
            throw new InvalidDataException("Unsupported/incomplete audio contract schema.");
        return catalog;
    }
    static Assembly FindAssembly(IEnumerable<Assembly> audio,string name)
    {
        var assemblies=audio.ToArray();var existing=assemblies.FirstOrDefault(a=>a.GetName().Name==name);if(existing!=null)return existing;
        var context=System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(assemblies[0])!;
        return context.LoadFromAssemblyName(new AssemblyName(name));
    }
    static Type FindType(Assembly assembly,string name) => assembly.GetType(name,true)!;
    static MemberInfo[] FindMembers(Type type,string kind,string name)
    {
        if(kind=="constructor")return type.GetConstructors(declared).Where(c=>!c.IsStatic).Cast<MemberInfo>().ToArray();
        if(kind=="method"||kind=="generated")return type.GetMethods(declared).Where(m=>kind=="generated"?m.Name.StartsWith(name,StringComparison.Ordinal):m.Name==name).Cast<MemberInfo>().ToArray();
        for(Type? cursor=type;cursor!=null;cursor=cursor.BaseType){
            MemberInfo? member=kind=="field"?cursor.GetField(name,declared):kind=="property"?cursor.GetProperty(name,declared):null;
            if(member!=null)return new[]{member};
        }
        return Array.Empty<MemberInfo>();
    }
    static string Signature(MemberInfo member,string? generatedName=null) => member switch {
        MethodBase m => (m.IsStatic?"static ":"instance ")+(m is ConstructorInfo?".ctor":generatedName??m.Name)+
            "("+string.Join(",",m.GetParameters().Select(p=>TypeKey(p.ParameterType)))+"):"+(m is MethodInfo mi?TypeKey(mi.ReturnType):"ctor")+
            "|generic="+(m is MethodInfo generic?generic.GetGenericArguments().Length:0)+"|virtual="+m.IsVirtual+"|abstract="+m.IsAbstract+"|visibility="+(m.Attributes&MethodAttributes.MemberAccessMask),
        FieldInfo f => (f.IsStatic?"static ":"instance ")+f.Name+":"+TypeKey(f.FieldType)+"|literal="+f.IsLiteral+"|readonly="+f.IsInitOnly+"|visibility="+(f.Attributes&FieldAttributes.FieldAccessMask)+(f.IsLiteral?"|value="+Convert.ToString(f.GetRawConstantValue(),System.Globalization.CultureInfo.InvariantCulture):""),
        PropertyInfo p => p.Name+":"+TypeKey(p.PropertyType)+"|index=("+string.Join(",",p.GetIndexParameters().Select(x=>TypeKey(x.ParameterType)))+")|get="+(p.GetMethod==null?"none":Signature(p.GetMethod))+"|set="+(p.SetMethod==null?"none":Signature(p.SetMethod)),
        _=>throw new NotSupportedException(member.ToString())
    };
    static string[] Signatures(Assembly assembly,MemberRule rule)
    {
        Type type=FindType(assembly,rule.Type);
        if(rule.Kind=="type")return new[]{TypeKey(type)+"|base="+(type.BaseType==null?"":TypeKey(type.BaseType))+"|attributes="+type.Attributes+"|interfaces="+string.Join(",",type.GetInterfaces().Select(TypeKey).Order(StringComparer.Ordinal))+"|enum="+(type.IsEnum?TypeKey(Enum.GetUnderlyingType(type)):"none")+"|custom="+Attributes(type)};
        var members=FindMembers(type,rule.Kind,rule.Name);
        // Harmony's lambda lookup uses Single; ambiguity is a hard structural failure.
        if(rule.Kind=="generated" && members.Length!=1)throw new NotSupportedException("Expected one generated method: "+rule.Id+"; found="+members.Length);
        if(members.Length==0)throw new MissingMemberException(rule.Type,rule.Name);
        return members.Select(m=>Signature(m,rule.Kind=="generated"?rule.Name:null)).Order(StringComparer.Ordinal).ToArray();
    }
    public sealed record IlInstruction(int Offset, OpCode OpCode, object? Operand);
    public static IReadOnlyList<IlInstruction> ReadInstructions(MethodBase method)
    {
        byte[] bytes=method.GetMethodBody()?.GetILAsByteArray() ?? throw new NotSupportedException("Method has no IL: "+method);
        var result=new List<IlInstruction>();int position=0;
        int I32(){int value=BitConverter.ToInt32(bytes,position);position+=4;return value;}
        while(position<bytes.Length){
            int offset=position;short code=bytes[position++];if(code==0xfe)code=(short)(0xfe00|bytes[position++]);
            if(!opcodes.TryGetValue(code,out OpCode op))throw new BadImageFormatException("Invalid opcode at "+offset);
            object? operand=null;
            switch(op.OperandType){
                case OperandType.InlineNone:break;
                case OperandType.ShortInlineI:operand=(int)(sbyte)bytes[position++];break;
                case OperandType.ShortInlineVar:operand=(int)bytes[position++];break;
                case OperandType.InlineVar:operand=(int)BitConverter.ToUInt16(bytes,position);position+=2;break;
                case OperandType.InlineI:operand=I32();break;
                case OperandType.InlineI8:operand=BitConverter.ToInt64(bytes,position);position+=8;break;
                case OperandType.ShortInlineR:operand=BitConverter.ToSingle(bytes,position);position+=4;break;
                case OperandType.InlineR:operand=BitConverter.ToDouble(bytes,position);position+=8;break;
                case OperandType.ShortInlineBrTarget:{int delta=(sbyte)bytes[position++];operand=position+delta;break;}
                case OperandType.InlineBrTarget:{int delta=I32();operand=position+delta;break;}
                case OperandType.InlineSwitch:{int count=I32();if(count<0||count>(bytes.Length-position)/4)throw new BadImageFormatException("Invalid switch");var deltas=new int[count];for(int i=0;i<count;i++)deltas[i]=I32();operand=deltas.Select(x=>position+x).ToArray();break;}
                case OperandType.InlineString:operand=method.Module.ResolveString(I32());break;
                case OperandType.InlineField:case OperandType.InlineMethod:case OperandType.InlineType:case OperandType.InlineTok:
                    operand=method.Module.ResolveMember(I32(),method.DeclaringType?.GetGenericArguments(),method.IsGenericMethod?method.GetGenericArguments():null);break;
                case OperandType.InlineSig:operand=Convert.ToHexString(method.Module.ResolveSignature(I32()));break;
                default:throw new BadImageFormatException("Unsupported IL operand: "+op.OperandType);
            }
            if(position>bytes.Length)throw new BadImageFormatException("Truncated IL");
            result.Add(new(offset,op,operand));
        }
        return result;
    }
    // A semantic projection keeps all control flow, numeric/enum arguments,
    // arithmetic, and exact metadata operands of critical methods. Only NOPs
    // are ignored, and branch targets are normalized to non-NOP ordinals. This
    // is deliberately stricter than a token-presence set: dead calls, swapped
    // BASS operations, changed flags and aggregate multiplication fail Tier 2.
    public static string[] SemanticInstructions(MethodBase method, int projectionRevision = 1)
    {
        var il=ReadInstructions(method);var meaningful=projectionRevision==2?SliceDiagnostics(method,il):il.Where(i=>i.OpCode!=OpCodes.Nop).ToArray();
        int Target(int offset){int index=Array.FindIndex(meaningful,i=>i.Offset>=offset);return index<0?meaningful.Length:index;}
        var body=method.GetMethodBody()!;
        var prefix=new List<string>{"method "+Signature(method),"body init="+body.InitLocals+" locals="+string.Join(",",body.LocalVariables.Select(v=>TypeKey(v.LocalType)+":"+v.IsPinned))};
        foreach(var clause in body.ExceptionHandlingClauses){
            string detail=clause.Flags==ExceptionHandlingClauseOptions.Clause?" catch="+TypeKey(clause.CatchType!):clause.Flags==ExceptionHandlingClauseOptions.Filter?" filter="+Target(clause.FilterOffset):"";
            prefix.Add("eh "+clause.Flags+" try="+Target(clause.TryOffset)+":"+Target(clause.TryOffset+clause.TryLength)+" handler="+Target(clause.HandlerOffset)+":"+Target(clause.HandlerOffset+clause.HandlerLength)+detail);
        }
        return prefix.Concat(meaningful.Select(instruction=>{
            OpCode op=instruction.OpCode;string name=op.Name!;object? operand=instruction.Operand;
            if(op.OperandType is OperandType.ShortInlineBrTarget or OperandType.InlineBrTarget)return name.Replace(".s","")+" ->"+Target((int)operand!);
            if(op.OperandType==OperandType.InlineSwitch)return "switch "+string.Join(",",((int[])operand!).Select(Target));
            return name+(operand==null?"": " "+(operand is MemberInfo member?MemberKey(member):operand is string text?JsonSerializer.Serialize(text):Convert.ToString(operand,System.Globalization.CultureInfo.InvariantCulture)));
        })).ToArray();
    }
    static MethodBase ResolveAnchor(Assembly assembly,AnchorRule rule)
    {
        Type type=FindType(assembly,rule.Type);
        string kind=rule.Method.StartsWith("prefix:",StringComparison.Ordinal)?"generated":rule.Method==".ctor"?"constructor":"method";
        string name=kind=="generated"?rule.Method[7..]:rule.Method;
        var methods=FindMembers(type,kind,name).OfType<MethodBase>().Where(m=>rule.Signature==null||Signature(m,kind=="generated"?name:null)==rule.Signature).ToArray();
        if(methods.Length!=1)throw new NotSupportedException("Expected one anchor method "+rule.Id+"; found="+methods.Length);
        return methods[0];
    }
    static string[] StructuralErrors(Assembly[] audio,PolicyProfile profile)
    {
        var errors=new List<string>();
        foreach(var rule in profile.Structure){try{var actual=Signatures(FindAssembly(audio,rule.Assembly),rule);if(!actual.SequenceEqual(rule.Signatures,StringComparer.Ordinal))errors.Add(rule.Id+": expected "+string.Join("; ",rule.Signatures)+"; actual "+string.Join("; ",actual));}catch(Exception e){errors.Add(rule.Id+": "+e.GetType().Name+" "+e.Message);}}
        return errors.ToArray();
    }
    static string[] SemanticErrors(Assembly[] audio,PolicyProfile profile)
    {
        var errors=new List<string>();
        ProjectionContext? context=null;
        if(profile.ProjectionRevision==2) {
            try{context=new ProjectionContext(audio,profile);}catch(Exception e){return new[]{"Generated anchor resolution: "+e.GetType().Name+" "+e.Message};}
        }
        foreach(var rule in profile.Anchors){try{
            var source=FindAssembly(audio,rule.Assembly);
            var actual=SemanticInstructions(context==null?ResolveAnchor(source,rule):context.Resolve(source,rule),profile.ProjectionRevision);
            if(context==null?!actual.SequenceEqual(rule.Instructions,StringComparer.Ordinal):!context.Matches(rule,actual))
                errors.Add(rule.Id+": critical IL/metadata/control-flow projection differs");
        }catch(Exception e){errors.Add(rule.Id+": "+e.GetType().Name+" "+e.Message);}}
        return errors.ToArray();
    }
    static bool Exact(Snapshot left,Snapshot right)=>left.Assembly==right.Assembly&&left.Types.Count==right.Types.Count&&left.Types.All(p=>right.Types.TryGetValue(p.Key,out var hash)&&hash==p.Value);
    public static CheckResult Evaluate(Assembly framework,Assembly game,ContractMode mode,Catalog? catalog=null)
        => EvaluateCore(framework,game,mode,catalog??EmbeddedCatalog,Inspect);
    internal static CheckResult EvaluateCore(Assembly framework,Assembly game,ContractMode mode,Catalog catalog,Func<Assembly,Snapshot> inspect)
    {
        if(mode is not (ContractMode.Strict or ContractMode.Adaptive))return new(MatchStatus.Failed,new[]{"Invalid Contract Mode"},Array.Empty<string>(),Array.Empty<string>(),null,null);
        var audio=new[]{framework,game};
        if(framework.GetName().Name!="osu.Framework"||game.GetName().Name!="osu.Game")return new(MatchStatus.Failed,new[]{"Expected one osu.Framework and one osu.Game assembly"},Array.Empty<string>(),Array.Empty<string>(),null,null);
        var attempts=new List<CheckResult>();
        foreach(var profile in catalog.Profiles){
            var structure=StructuralErrors(audio,profile);
            if(structure.Length>0){attempts.Add(new(MatchStatus.Failed,structure,Array.Empty<string>(),Array.Empty<string>(),profile.Id,null));continue;}
            var semantics=SemanticErrors(audio,profile);
            if(semantics.Length>0){attempts.Add(new(MatchStatus.Failed,Array.Empty<string>(),semantics,Array.Empty<string>(),profile.Id,null));continue;}
            Snapshot actualFramework,actualGame;
            try{actualFramework=inspect(framework);actualGame=inspect(game);}catch(Exception e){attempts.Add(new(MatchStatus.Failed,Array.Empty<string>(),Array.Empty<string>(),new[]{"Cannot compute exact contract: "+e.Message},profile.Id,null));continue;}
            var reviewed=catalog.ReviewedContracts.FirstOrDefault(r=>r.Policy==profile.Id&&Exact(r.Framework,actualFramework)&&Exact(r.Game,actualGame));
            if(reviewed!=null)return new(MatchStatus.ExactMatch,Array.Empty<string>(),Array.Empty<string>(),Array.Empty<string>(),profile.Id,reviewed.Id);
            string[] differences=catalog.ReviewedContracts.Where(r=>r.Policy==profile.Id).SelectMany(r=>new[]{r.Framework,r.Game}).SelectMany(r=>r.Types.Where(p=>!(r.Assembly=="osu.Framework"?actualFramework:actualGame).Types.TryGetValue(p.Key,out string? hash)||hash!=p.Value).Select(p=>p.Key)).Distinct().ToArray();
            if(differences.Length==0)differences=new[]{"Game/Framework combination is not reviewed"};
            attempts.Add(new(mode==ContractMode.Adaptive?MatchStatus.AdaptiveMatch:MatchStatus.Failed,Array.Empty<string>(),Array.Empty<string>(),differences,profile.Id,null));
        }
        return attempts.FirstOrDefault(r=>r.Status==MatchStatus.AdaptiveMatch)??attempts.OrderBy(r=>r.StructuralErrors.Length>0?2:r.SemanticErrors.Length>0?1:0).First();
    }
    public static void RequireAccepted(CheckResult result)
    {
        if(!result.Accepted)throw new NotSupportedException("[Contract Failed] "+string.Join(" | ",result.StructuralErrors.Select(x=>"Tier1 "+x).Concat(result.SemanticErrors.Select(x=>"Tier2 "+x)).Concat(result.ExactDifferences.Select(x=>"Tier3 "+x))));
    }
    public static CheckResult ValidatePair(Assembly framework,Assembly game,ContractMode mode=ContractMode.Strict)
    {
        var result=Evaluate(framework,game,mode);RequireAccepted(result);return result;
    }
    public static PolicyProfile CapturePolicy(Assembly framework,Assembly game,PolicyProfile template)
    {
        var audio=new[]{framework,game};
        var context=template.ProjectionRevision==2?new ProjectionContext(audio,template):null;
        return template with {
            Structure=template.Structure.Select(rule=>rule with {Signatures=Signatures(FindAssembly(audio,rule.Assembly),rule)}).ToArray(),
            Anchors=template.Anchors.Select(rule=>rule with {Instructions=SemanticInstructions(context==null?ResolveAnchor(FindAssembly(audio,rule.Assembly),rule):context.Resolve(FindAssembly(audio,rule.Assembly),rule),template.ProjectionRevision)}).ToArray()
        };
    }
    // Used only by the maintenance extractor to capture an observed baseline.
    // Runtime validation never learns from an unreviewed client.
    internal static PolicyProfile ObservePolicy(Assembly framework,Assembly game,string id="audio-v1")
    {
        var members=new List<MemberRule>();var anchors=new List<AnchorRule>();
        void Member(string type,string kind,string name){string assembly=type.StartsWith("osu.Game.",StringComparison.Ordinal)?"osu.Game":type.StartsWith("ManagedBass.",StringComparison.Ordinal)?"ppy.ManagedBass":"osu.Framework";var rule=new MemberRule(type+"::"+kind+":"+name,assembly,type,kind,name,Array.Empty<string>());members.Add(rule with {Signatures=Signatures(FindAssembly(new[]{framework,game},assembly),rule)});}
        void Anchor(string type,string method){string assembly=type.StartsWith("osu.Game.",StringComparison.Ordinal)?"osu.Game":"osu.Framework";var source=assembly=="osu.Game"?game:framework;string kind=method==".ctor"?"constructor":method.StartsWith("prefix:")?"generated":"method";string name=kind=="generated"?method[7..]:method;var targets=FindMembers(FindType(source,type),kind,name).OfType<MethodBase>().ToArray();if(targets.Length==0)throw new MissingMethodException(type,method);foreach(var target in targets){string signature=Signature(target,kind=="generated"?name:null);anchors.Add(new(type+"::"+signature,assembly,type,method,SemanticInstructions(target),signature));}}
        const string adjustments="osu.Framework.Audio.AudioAdjustments",component="osu.Framework.Audio.AdjustableAudioComponent",manager="osu.Framework.Audio.AudioManager",aggregate="osu.Framework.Bindables.AggregateBindable`1",wrapper="osu.Framework.Graphics.Audio.DrawableAudioWrapper",mixer="osu.Framework.Graphics.Audio.DrawableAudioMixer",channel="osu.Framework.Audio.Sample.SampleChannelBass",bassMixer="osu.Framework.Audio.Mixing.Bass.BassAudioMixer",sound="osu.Game.Skinning.SkinnableSound",pooled="osu.Game.Skinning.PoolableSkinnableSample";
        foreach(string type in frameworkTypes.Where(t=>framework.GetType(t)!=null).Concat(gameTypes))Member(type,"type","");
        foreach(var spec in new[]{(adjustments,"constructor",""),(adjustments,"field","volumeAggregate"),(adjustments,"property","AggregateVolume"),(adjustments,"property","Volume"),(aggregate,"field","sourceMapping"),(aggregate,"field","initialValue"),(aggregate,"property","Result"),(aggregate+"+WeakRefPair","property","WeakReference"),("osu.Framework.Bindables.Bindable`1","field","ValueChanged"),("osu.Framework.Bindables.Bindable`1","property","Value"),(component,"field","adjustments"),(component,"property","AggregateVolume"),(manager,"constructor",""),(manager,"property","Volume"),(wrapper,"field","adjustments"),(wrapper,"field","component"),(wrapper,"property","Parent"),(wrapper,"property","AggregateVolume"),(mixer,"method","load"),(mixer,"field","Name"),(mixer,"property","Volume"),(channel,"field","channel"),(channel,"method","OnStateChanged"),(channel,"method","Play"),(channel,"generated","<ensureChannel>b__"),(channel,"property","Mixer"),(channel,"property","AggregateVolume"),(bassMixer,"field","manager"),(bassMixer,"field","Identifier"),(sound,"property","PlayWhenZeroVolume"),(sound,"property","Looping"),(sound,"field","samplesContainer"),(sound,"generated","<Play>b__"),(pooled,"field","sampleContainer"),(pooled,"property","Sample")})Member(spec.Item1,spec.Item2,spec.Item3);
        Member("osu.Game.Rulesets.UI.DrawableRuleset`1","type","");
        Member("osu.Game.Rulesets.UI.DrawableRuleset`1","field","gameplayMixer");
        Member("osu.Game.Rulesets.UI.DrawableRuleset`1","field","hitsoundVolume");
        // Enum values actually used in the native-facing mix/attribute contract.
        foreach(string name in new[]{"Volume","Balance","Frequency","Tempo"})Member("osu.Framework.Audio.AdjustableProperty","field",name);
        foreach(var spec in new[]{("ManagedBass.PlaybackState",new[]{"Stopped","Playing","Stalled","Paused"}),("ManagedBass.ChannelAttribute",new[]{"Volume","Pan","Frequency","Buffer"}),("ManagedBass.BassFlags",new[]{"MixerChanPause","MixerChanBuffer","MixerChanNoRampin","SampleChannelStream","Decode","Loop","MixerNonStop","Default","Float"})}){Member(spec.Item1,"type","");foreach(string name in spec.Item2)Member(spec.Item1,"field",name);}
        // Transitive semantic anchors: critical native calls, the zero-volume
        // gate, aggregate source topology and multiplication, mixer identity.
        foreach(var spec in new[]{(channel,"OnStateChanged"),(channel,"Play"),(channel,"prefix:<Play>b__"),(channel,"prefix:<ensureChannel>b__"),(channel,"playInternal"),(bassMixer,"ChannelPlay"),(bassMixer,"ChannelPause"),(bassMixer,"ChannelIsActive"),(bassMixer,"AddChannelToBassMix"),(bassMixer,"createMixer"),(bassMixer,".ctor"),("osu.Framework.Audio.Mixing.AudioMixer",".ctor"),(manager,".ctor"),(manager,"CreateAudioMixer"),(manager,"createAudioMixer"),(mixer,"load"),(adjustments,"get_AggregateVolume"),(adjustments,".ctor"),(adjustments,"getAggregate"),(aggregate,".ctor"),(component,"get_Adjustments"),(aggregate,"recalculateAggregate"),(sound,"get_PlayWhenZeroVolume"),(sound,"prefix:<Play>b__"),(pooled,"Play"),("osu.Game.Rulesets.UI.DrawableRuleset`1",".ctor"),("osu.Game.Rulesets.UI.DrawableRuleset`1","load"),("osu.Game.Rulesets.UI.DrawableRuleset`1","get_Audio"),("osu.Framework.Graphics.Audio.DrawableSample","GetChannel"),("osu.Framework.Graphics.Audio.DrawableSample","OnMixerChanged"),(wrapper,"refreshLayoutFromParent"),("osu.Framework.Audio.Sample.Sample","GetChannel"),("osu.Framework.Audio.Mixing.AudioMixer","Add"),("osu.Framework.Audio.Mixing.AudioMixer+<>c__DisplayClass3_0","<Add>b__0"),(channel,"ensureChannel"),(component,"get_AggregateVolume"),(wrapper,"get_Volume"),(wrapper,"get_AggregateVolume")})Anchor(spec.Item1,spec.Item2);
        // Include the volume aggregate constructor/function and its generated
        // arithmetic lambda. Compiler-generated helper names are captured in
        // this reviewed policy; an unexpected new layout fails closed.
        foreach(MethodInfo method in FindType(framework,adjustments).GetMethods(declared).Where(m=>m.Name=="getAggregateFunction"))anchors.Add(new(adjustments+"::"+method.Name,"osu.Framework",adjustments,method.Name,SemanticInstructions(method)));
        Type? closure=FindType(framework,adjustments).GetNestedTypes(declared).FirstOrDefault(t=>t.Name=="<>c");
        if(closure==null)throw new MissingMemberException("AudioAdjustments aggregate lambda type");
        foreach(MethodInfo method in closure.GetMethods(declared).Where(m=>m.Name.StartsWith("<getAggregateFunction>",StringComparison.Ordinal)))anchors.Add(new(closure.FullName+"::"+method.Name,"osu.Framework",closure.FullName!,method.Name,SemanticInstructions(method)));
        return new(id,members.ToArray(),anchors.ToArray());
    }
}
