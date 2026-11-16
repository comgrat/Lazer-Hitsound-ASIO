using Mono.Cecil;
using Mono.Cecil.Cil;
using System.Security.Cryptography;

namespace AudioContractProbe;
internal static class ContractMutations
{
    public sealed record Mutation(string Name,string? Framework,string? Game,bool Adaptive,string? Bass = null);
    static IEnumerable<TypeDefinition> Types(IEnumerable<TypeDefinition> roots)=>roots.SelectMany(t=>new[]{t}.Concat(Types(t.NestedTypes)));
    static string Hash(string file)=>Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)));
    public static IEnumerable<Mutation> Create(string directory,string output)
    {
        var definitions=new (string Name,string Assembly,bool Adaptive,Action<ModuleDefinition> Edit)[]{
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
