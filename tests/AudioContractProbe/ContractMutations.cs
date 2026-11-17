using Mono.Cecil;
using Mono.Cecil.Cil;
using System.Security.Cryptography;

namespace AudioContractProbe;
internal static class ContractMutations
{
    public sealed record Mutation(string Name,string? Framework,string? Game,bool Adaptive,string? Bass = null);
    static IEnumerable<TypeDefinition> Types(IEnumerable<TypeDefinition> roots)=>roots.SelectMany(t=>new[]{t}.Concat(Types(t.NestedTypes)));
    static string Hash(string file)=>Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)));
    static void RenameGenerated(ModuleDefinition module,bool arithmetic=false,bool swap=false,bool duplicate=false)
    {
        var closure=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.AudioAdjustments/<>c");
        var multiply=closure.Methods.Single(m=>m.Name.StartsWith("<getAggregateFunction>")&&m.Body.Instructions.Any(i=>i.OpCode==OpCodes.Mul));
        foreach(var m in closure.Methods.Where(m=>m.Name.StartsWith("<getAggregateFunction>")))m.Name=m.Name.Replace("b__33_","b__71_");
        foreach(var f in closure.Fields.Where(f=>f.Name.StartsWith("<>9__33_")))f.Name=f.Name.Replace("<>9__33_","<>9__71_");
        var display=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.Mixing.AudioMixer/<>c__DisplayClass3_0");
        display.Name="<>c__DisplayClass88_0";
        display.Methods.Single(m=>m.Name=="<Add>b__0").Name="<Add>b__42_0";
        if(arithmetic)multiply.Body.Instructions.Single(i=>i.OpCode==OpCodes.Mul).OpCode=OpCodes.Add;
        if(swap){
            var functions=closure.Methods.Where(m=>m.Name.StartsWith("<getAggregateFunction>")).ToArray();
            var method=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.AudioAdjustments").Methods.Single(m=>m.Name=="getAggregateFunction");
            foreach(var instruction in method.Body.Instructions.Where(i=>i.OpCode==OpCodes.Ldftn)){
                if(instruction.Operand is MethodReference reference){var index=Array.FindIndex(functions,m=>m.Name==reference.Name);if(index>=0)instruction.Operand=functions[1-index];}
            }
        }
        if(duplicate){
            var clone=new MethodDefinition("<getAggregateFunction>b__99_0",multiply.Attributes,multiply.ReturnType);
            foreach(var p in multiply.Parameters)clone.Parameters.Add(new ParameterDefinition(p.ParameterType));
            foreach(var i in multiply.Body.Instructions)clone.Body.Instructions.Add(Instruction.Create(OpCodes.Nop));
            // This arithmetic lambda consists entirely of argument loads, mul, ret.
            for(int i=0;i<multiply.Body.Instructions.Count;i++){clone.Body.Instructions[i].OpCode=multiply.Body.Instructions[i].OpCode;clone.Body.Instructions[i].Operand=multiply.Body.Instructions[i].Operand;}
            clone.Body.InitLocals=multiply.Body.InitLocals;closure.Methods.Add(clone);
        }
    }
    static void AddDiagnostic(ModuleDefinition module,bool branchInto=false,bool changedGain=false)
    {
        var method=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.AudioManager").Methods.First(m=>m.IsConstructor&&!m.IsStatic);
        var first=method.Body.Instructions[0];var il=method.Body.GetILProcessor();
        var text=Instruction.Create(OpCodes.Ldstr,"contract probe diagnostic");
        var call=Instruction.Create(OpCodes.Call,module.ImportReference(typeof(Console).GetMethod(nameof(Console.WriteLine),new[]{typeof(string)})!));
        il.InsertBefore(first,text);il.InsertBefore(first,call);
        if(branchInto)il.InsertBefore(text,Instruction.Create(OpCodes.Br,text));
        if(changedGain){il.InsertBefore(first,Instruction.Create(OpCodes.Ldc_I4_0));il.InsertBefore(first,Instruction.Create(OpCodes.Pop));}
    }
    public static IEnumerable<Mutation> Create(string directory,string output)
    {
        var definitions=new (string Name,string Assembly,bool Adaptive,Action<ModuleDefinition> Edit)[]{
            ("renumbered-generated-contexts","osu.Framework",true,m=>RenameGenerated(m)),
            ("renumbered-changed-arithmetic","osu.Framework",false,m=>RenameGenerated(m,arithmetic:true)),
            ("renumbered-swapped-call-targets","osu.Framework",false,m=>RenameGenerated(m,swap:true)),
            ("ambiguous-generated-body","osu.Framework",false,m=>RenameGenerated(m,duplicate:true)),
            ("harmless-entry-diagnostic","osu.Framework",true,m=>AddDiagnostic(m)),
            ("diagnostic-with-branch-entry","osu.Framework",false,m=>AddDiagnostic(m,branchInto:true)),
            ("diagnostic-with-extra-computation","osu.Framework",false,m=>AddDiagnostic(m,changedGain:true)),
            ("harmless-nop-in-anchor","osu.Game",true,module=>{var method=Types(module.Types).Single(t=>t.FullName=="osu.Game.Skinning.SkinnableSound").Methods.Single(m=>m.Name=="get_PlayWhenZeroVolume");method.Body.GetILProcessor().InsertBefore(method.Body.Instructions[0],Instruction.Create(OpCodes.Nop));}),
            ("harmless-unrelated-helper","osu.Framework",true,module=>{var type=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.AudioAdjustments");var helper=new MethodDefinition("ContractProbe_Unrelated",MethodAttributes.Private|MethodAttributes.Static,module.TypeSystem.Void);helper.Body.GetILProcessor().Emit(OpCodes.Ret);type.Methods.Add(helper);}),
            ("broken-channel-field-type","osu.Framework",false,module=>Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.Sample.SampleChannelBass").Fields.Single(f=>f.Name=="channel").FieldType=module.TypeSystem.Int64),
            ("broken-onstate-signature","osu.Framework",false,module=>Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.Sample.SampleChannelBass").Methods.Single(m=>m.Name=="OnStateChanged").Parameters.Add(new ParameterDefinition(module.TypeSystem.Int32))),
            ("broken-zero-volume-call","osu.Game",false,module=>{var type=Types(module.Types).Single(t=>t.FullName=="osu.Game.Skinning.SkinnableSound");var getter=type.Methods.Single(m=>m.Name=="get_PlayWhenZeroVolume");getter.Body.Instructions.Clear();getter.Body.ExceptionHandlers.Clear();var il=getter.Body.GetILProcessor();il.Emit(OpCodes.Ldc_I4_0);il.Emit(OpCodes.Ret);}),
            ("broken-mixer-pause-direction","osu.Framework",false,module=>{var type=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.Mixing.Bass.BassAudioMixer");var play=type.Methods.Single(m=>m.Name=="ChannelPlay");var pause=type.Methods.Single(m=>m.Name=="ChannelPause");var wrong=pause.Body.Instructions.Select(i=>i.Operand).OfType<MethodReference>().Single(m=>m.Name=="ChannelAddFlag");var call=play.Body.Instructions.Single(i=>i.Operand is MethodReference m&&m.Name=="ChannelRemoveFlag");call.Operand=wrong;}),
            ("broken-audiomanager-ctor","osu.Framework",false,module=>{var ctor=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.AudioManager").Methods.First(m=>m.IsConstructor&&!m.IsStatic);var il=ctor.Body.GetILProcessor();il.InsertBefore(ctor.Body.Instructions[0],Instruction.Create(OpCodes.Ldc_I4_0));il.InsertBefore(ctor.Body.Instructions[0].Next,Instruction.Create(OpCodes.Pop));}),
            ("broken-mixer-creation","osu.Framework",false,module=>{var method=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.Mixing.Bass.BassAudioMixer").Methods.Single(m=>m.Name=="createMixer");method.Body.ExceptionHandlers.Clear();method.Body.Instructions.Clear();method.Body.GetILProcessor().Emit(OpCodes.Ret);}),
            ("broken-gameplay-name","osu.Game",false,module=>{var type=Types(module.Types).Single(t=>t.FullName=="osu.Game.Rulesets.UI.DrawableRuleset`1");var instruction=type.Methods.Where(m=>m.IsConstructor&&m.HasBody).SelectMany(m=>m.Body.Instructions).Single(i=>i.OpCode==OpCodes.Ldstr&&(string)i.Operand=="GameplayMixer");instruction.Operand="OtherMixer";}),
            ("broken-mixer-identifier-chain","osu.Framework",false,module=>{var type=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.Mixing.AudioMixer");var ctor=type.Methods.Single(m=>m.IsConstructor&&!m.IsStatic);var il=ctor.Body.GetILProcessor();var instruction=ctor.Body.Instructions.Single(i=>i.OpCode==OpCodes.Stfld&&i.Operand is FieldReference f&&f.Name=="Identifier");il.InsertBefore(instruction,Instruction.Create(OpCodes.Pop));il.InsertBefore(instruction,Instruction.Create(OpCodes.Ldstr,"OtherMixer"));}),
            ("broken-interface-shape","osu.Framework",false,module=>{var type=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.Mixing.Bass.BassAudioMixer");var contract=type.Interfaces.Single(i=>i.InterfaceType.FullName=="osu.Framework.Audio.IBassAudio");type.Interfaces.Remove(contract);}),
            ("broken-enum-underlying","ppy.ManagedBass",false,module=>{var type=Types(module.Types).Single(t=>t.FullName=="ManagedBass.BassFlags");type.Fields.Single(f=>f.Name=="value__").FieldType=module.TypeSystem.Int64;foreach(var field in type.Fields.Where(f=>f.IsLiteral))field.Constant=Convert.ToInt64(field.Constant);}),
            ("broken-enum-buffer","ppy.ManagedBass",false,module=>Types(module.Types).Single(t=>t.FullName=="ManagedBass.ChannelAttribute").Fields.Single(f=>f.Name=="Buffer").Constant=999),
            ("broken-aggregate-arithmetic","osu.Framework",false,module=>{var type=Types(module.Types).Single(t=>t.FullName=="osu.Framework.Audio.AudioAdjustments/<>c");var method=type.Methods.Single(m=>m.Name.StartsWith("<getAggregateFunction>")&&m.Body.Instructions.Any(i=>i.OpCode==OpCodes.Mul));method.Body.Instructions.Single(i=>i.OpCode==OpCodes.Mul).OpCode=OpCodes.Add;})
        };
        foreach(var definition in definitions){
            string source=Path.Combine(directory,definition.Assembly+".dll");string before=Hash(source);
            string targetDirectory=Path.Combine(output,definition.Name);Directory.CreateDirectory(targetDirectory);
            string target=Path.Combine(targetDirectory,definition.Assembly+".dll");
            var resolver=new DefaultAssemblyResolver();resolver.AddSearchDirectory(directory);resolver.AddSearchDirectory(Path.GetDirectoryName(typeof(object).Assembly.Location)!);
            using(var module=ModuleDefinition.ReadModule(source,new ReaderParameters{AssemblyResolver=resolver,InMemory=true,ReadWrite=false})){definition.Edit(module);module.Write(target);}
            if(Hash(source)!=before)throw new InvalidOperationException("Source client changed during mutation");
            yield return new(definition.Name,definition.Assembly=="osu.Framework"?target:null,definition.Assembly=="osu.Game"?target:null,definition.Adaptive,definition.Assembly=="ppy.ManagedBass"?target:null);
        }
    }
}
