using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.Loader;
using System.Text.Json;

namespace AudioContractProbe;
internal static class TieredRegression
{
    static int checks;
    static readonly List<object> cases=new();
    static void Check(bool value,string message){checks++;if(!value)throw new InvalidOperationException(message);Console.WriteLine("PASS "+message);}
    static Assembly Build(string assemblyName,bool drift=false,bool signatureBreak=false,bool anchorBreak=false,bool reorder=false)
    {
        var assembly=AssemblyBuilder.DefineDynamicAssembly(new AssemblyName(assemblyName),AssemblyBuilderAccess.RunAndCollect);
        var module=assembly.DefineDynamicModule(assemblyName);var type=module.DefineType(assemblyName+".Fixture",TypeAttributes.Public);
        var field=type.DefineField("handle",signatureBreak?typeof(long):typeof(int),FieldAttributes.Public);
        var play=type.DefineMethod("Play",MethodAttributes.Public|MethodAttributes.Static,typeof(void),Type.EmptyTypes);
        var il=play.GetILGenerator();if(drift)il.Emit(OpCodes.Nop);
        if(reorder){il.Emit(OpCodes.Ldc_I4_2);il.Emit(OpCodes.Pop);il.Emit(OpCodes.Ldc_I4_1);il.Emit(OpCodes.Pop);}
        else{il.Emit(OpCodes.Ldc_I4_1);il.Emit(OpCodes.Pop);il.Emit(OpCodes.Ldc_I4_2);il.Emit(OpCodes.Pop);}
        if(anchorBreak){il.Emit(OpCodes.Ldc_I4_0);il.Emit(OpCodes.Pop);}
        il.Emit(OpCodes.Ret);type.CreateType();return assembly;
    }
    static string Signature(Assembly assembly)=>assembly.GetType(assembly.GetName().Name+".Fixture")!.GetField("handle")!.FieldType.FullName!;
    public static int Run(string[] args)
    {
        checks=0;cases.Clear();
        ConfigAndSession();
        // In-memory assemblies isolate the evaluator from any installed client.
        var framework=Build("osu.Framework");var game=Build("osu.Game");
        var member=new AudioContract.MemberRule("fixture.handle","osu.Framework","osu.Framework.Fixture","field","handle",new[]{"instance handle:[System.Private.CoreLib]System.Int32|literal=False|readonly=False|visibility=Public"});
        var method=framework.GetType("osu.Framework.Fixture")!.GetMethod("Play")!;
        var anchor=new AudioContract.AnchorRule("fixture.play","osu.Framework","osu.Framework.Fixture","Play",AudioContract.SemanticInstructions(method));
        // Use the actual structural/semantic evaluator with injectable exact
        // snapshots. Real-client cases below additionally use production hashes.
        var policy=new AudioContract.PolicyProfile("test",new[]{member},new[]{anchor});
        AudioContract.Snapshot InspectFixture(Assembly assembly)=>AudioContract.InspectTypes(assembly,new[]{assembly.GetName().Name+".Fixture"});
        var catalog=new AudioContract.Catalog(2,new[]{policy},new[]{new AudioContract.ReviewedContract("fixture-reviewed",policy.Id,InspectFixture(framework),InspectFixture(game))});
        foreach(var item in new[]{(Name:"exact",Assembly:framework,Structure:true,Semantics:true),(Name:"nop-drift",Assembly:Build("osu.Framework",drift:true),Structure:true,Semantics:true),(Name:"field-type",Assembly:Build("osu.Framework",signatureBreak:true),Structure:false,Semantics:true),(Name:"anchor-removed",Assembly:Build("osu.Framework",anchorBreak:true),Structure:true,Semantics:false),(Name:"call-order",Assembly:Build("osu.Framework",reorder:true),Structure:true,Semantics:false)}){
            var captured=AudioContract.CapturePolicy(item.Assembly,game,policy);
            bool structure=captured.Structure[0].Signatures.SequenceEqual(member.Signatures);
            bool semantics=captured.Anchors[0].Instructions.SequenceEqual(anchor.Instructions);
            Check(structure==item.Structure,item.Name+" structural expectation");Check(semantics==item.Semantics,item.Name+" semantic expectation");
            var strict=AudioContract.EvaluateCore(item.Assembly,game,AudioContract.ContractMode.Strict,catalog,InspectFixture);
            var adaptive=AudioContract.EvaluateCore(item.Assembly,game,AudioContract.ContractMode.Adaptive,catalog,InspectFixture);
            Check(strict.Accepted==(item.Name=="exact"),item.Name+" full Strict decision");
            Check(adaptive.Accepted==(item.Structure&&item.Semantics),item.Name+" full Adaptive decision");
            if(item.Name=="nop-drift")Check(adaptive.Status==AudioContract.MatchStatus.AdaptiveMatch,"NOP drift reports AdaptiveMatch");
            if(!item.Structure)Check(adaptive.StructuralErrors.Length>0,"broken signature fails Tier1");
            if(item.Structure&&!item.Semantics)Check(adaptive.SemanticErrors.Length>0,"broken anchor/order fails Tier2");
        }
        // Exercise full three-tier decisions using source-built fixture types
        // under a catalog that explicitly declares their inspected type roots.
        // The integration command needs a compatible client and mutates copies.
        if(args.Length>0)return RealCases(args[0],args.Length>1?args[1]:Path.Combine(Path.GetTempPath(),"AsioContract_"+Guid.NewGuid()));
        var revised=policy with {ProjectionRevision=2};
        var revisedCatalog=catalog with {Profiles=new[]{revised}};
        Check(AudioContract.EvaluateCore(framework,game,AudioContract.ContractMode.Strict,revisedCatalog,InspectFixture).Accepted,"revision2 synthetic exact accepts");
        Check(!AudioContract.EvaluateCore(Build("osu.Framework",reorder:true),game,AudioContract.ContractMode.Adaptive,revisedCatalog,InspectFixture).Accepted,"revision2 synthetic changed order rejects");
        Check(!AudioContract.EvaluateCore(Build("osu.Framework",anchorBreak:true),game,AudioContract.ContractMode.Adaptive,revisedCatalog,InspectFixture).Accepted,"revision2 synthetic extra computation rejects");
        Console.WriteLine("Tiered fixture projections checks="+checks+" failures=0");return 0;
    }
    static (Assembly F,Assembly G,AssemblyLoadContext Context) Load(string client,string? frameworkOverride=null,string? gameOverride=null,string? bassOverride=null)
    {
        var context=new AssemblyLoadContext("tiered-tests:"+Guid.NewGuid(),true);
        context.Resolving+=(_,name)=>{string file=name.Name=="ppy.ManagedBass"&&bassOverride!=null?bassOverride:Path.Combine(client,name.Name+".dll");return File.Exists(file)?context.LoadFromAssemblyPath(file):null;};
        return(context.LoadFromAssemblyPath(frameworkOverride??Path.Combine(client,"osu.Framework.dll")),context.LoadFromAssemblyPath(gameOverride??Path.Combine(client,"osu.Game.dll")),context);
    }
    static void ConfigAndSession()
    {
        string root=Path.Combine(Path.GetTempPath(),"AsioContractSettings_"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        string file=Path.Combine(root,"config.ini");
        Check(ContractSettings.Read(root)==AudioContract.ContractMode.Adaptive,"missing config defaults Adaptive");
        File.WriteAllText(file,"[Config]\nBuffer Size=16\nASIO Driver ID=0\nSample Rate=48000\nOutput Volume=50\nDebug Log=0\n");
        Check(ContractSettings.Read(root)==AudioContract.ContractMode.Adaptive,"legacy five fields default Adaptive");
        File.AppendAllText(file,"Contract Mode = 0 ; reviewed only\n");Check(ContractSettings.Read(root)==AudioContract.ContractMode.Strict,"explicit Strict parsed");
        File.WriteAllText(file,"Contract Mode=2\n");bool invalid=false;try{ContractSettings.Read(root);}catch(InvalidDataException){invalid=true;}Check(invalid,"invalid mode rejects");
        File.WriteAllText(file,"Contract Mode=1\nContract Mode=0\n");invalid=false;try{ContractSettings.Read(root);}catch(InvalidDataException){invalid=true;}Check(invalid,"duplicate mode rejects");
        using(var mapping=System.IO.MemoryMappedFiles.MemoryMappedFile.CreateNew("Local\\OsuAsioLive_"+Environment.ProcessId,24))
        using(var view=mapping.CreateViewAccessor()){
            view.Write(0,0x41534c32);view.Write(4,Environment.ProcessId);view.Write(16,0);view.Flush();
            using var session=ContractSession.Open(Environment.ProcessId);Check(session!=null&&session.Mode==AudioContract.ContractMode.Strict,"native ABI offsets freeze Strict");
            File.WriteAllText(file,"Contract Mode=1\n");Check(session!.Mode==AudioContract.ContractMode.Strict,"INI edits do not alter frozen session mode");
            session.Publish(AudioContract.MatchStatus.AdaptiveMatch);Check(view.ReadInt32(20)==2,"Adaptive result published through native ABI");
            session.Publish(AudioContract.MatchStatus.ExactMatch);Check(view.ReadInt32(20)==1,"Exact result published through native ABI");
        }
    }
    static int RealCases(string directory,string output)
    {
        directory=Path.GetFullPath(directory);output=Path.GetFullPath(output);Directory.CreateDirectory(output);
        var loaded=Load(directory);
        try{
            var exact=AudioContract.Evaluate(loaded.F,loaded.G,AudioContract.ContractMode.Strict);
            Check(exact.Status==AudioContract.MatchStatus.ExactMatch,"real reviewed pair Strict ExactMatch");
            Check(AudioContract.Evaluate(loaded.F,loaded.G,AudioContract.ContractMode.Adaptive).Status==AudioContract.MatchStatus.ExactMatch,"real reviewed pair Adaptive ExactMatch");
            cases.Add(new{name="exact",status=exact.Status.ToString()});
        }finally{loaded.Context.Unload();}
        // Actual PE/IL mutations are supplied by the shared metadata writer.
        foreach(var mutation in ContractMutations.Create(directory,output)){
            var target=Load(directory,mutation.Framework,mutation.Game,mutation.Bass);
            try{
                var strict=AudioContract.Evaluate(target.F,target.G,AudioContract.ContractMode.Strict);
                var adaptive=AudioContract.Evaluate(target.F,target.G,AudioContract.ContractMode.Adaptive);
                Check(!strict.Accepted,mutation.Name+" Strict rejects");
                if(adaptive.Accepted!=mutation.Adaptive)Console.WriteLine("CONTRACT_DETAIL "+JsonSerializer.Serialize(adaptive,AudioContract.JsonOptions));
                Check(adaptive.Accepted==mutation.Adaptive,mutation.Name+" Adaptive expectation");
                if(mutation.Adaptive)Check(adaptive.Status==AudioContract.MatchStatus.AdaptiveMatch,mutation.Name+" Adaptive compatibility status");
                else Check(adaptive.StructuralErrors.Length+adaptive.SemanticErrors.Length>0,mutation.Name+" fails before exact tier");
                cases.Add(new{name=mutation.Name,strict,adaptive});
            }finally{target.Context.Unload();}
        }
        File.WriteAllText(Path.Combine(output,"tiered-results.json"),JsonSerializer.Serialize(new{checks,failures=0,cases},AudioContract.JsonOptions));
        Console.WriteLine("TieredAudioContract real regression checks="+checks+" failures=0");return 0;
    }
}
