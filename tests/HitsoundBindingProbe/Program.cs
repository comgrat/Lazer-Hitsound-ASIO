using Mono.Cecil;
using Mono.Cecil.Cil;
using System.Security.Cryptography;
if(args.Length!=1)return 2;
string directory=Path.GetFullPath(args[0]);
IEnumerable<TypeDefinition> Types(IEnumerable<TypeDefinition> roots)=>roots.SelectMany(t=>new[]{t}.Concat(Types(t.NestedTypes)));
int? Constant(Instruction i)=>i.OpCode.Code switch {Code.Ldc_I4=>(int)i.Operand,Code.Ldc_I4_S=>Convert.ToInt32(i.Operand),Code.Ldc_I4_0=>0,Code.Ldc_I4_1=>1,Code.Ldc_I4_2=>2,Code.Ldc_I4_3=>3,Code.Ldc_I4_4=>4,Code.Ldc_I4_5=>5,Code.Ldc_I4_6=>6,Code.Ldc_I4_7=>7,Code.Ldc_I4_8=>8,_=>null};
string file=Path.Combine(directory,"osu.Game.dll");string hash=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)));
var resolver=new DefaultAssemblyResolver();resolver.AddSearchDirectory(directory);resolver.AddSearchDirectory(Path.GetDirectoryName(typeof(object).Assembly.Location)!);
using var module=ModuleDefinition.ReadModule(file,new ReaderParameters{AssemblyResolver=resolver,InMemory=true,ReadWrite=false});
var settings=Types(module.Types).Single(t=>t.FullName=="osu.Game.Configuration.OsuSetting");
var hitsound=settings.Fields.Single(f=>f.Name=="HitsoundVolume");int value=Convert.ToInt32(hitsound.Constant);
Console.WriteLine($"Game={module.Assembly.Name.Version} SHA256={hash} HitsoundVolume={value}");
int matches=0;
foreach(var type in Types(module.Types))foreach(var method in type.Methods.Where(m=>m.HasBody)){
    var il=method.Body.Instructions;
    bool relevant=il.Any(i=>i.Operand?.ToString()?.Contains("HitsoundVolume")==true);
    for(int n=0;n<il.Count && !relevant;n++)if(Constant(il[n])==value){
        for(int j=n+1;j<Math.Min(n+10,il.Count);j++)if(il[j].Operand is MethodReference called && (called.Name.Contains("Bindable")||called.Name.Contains("Get")||called.Name.Contains("Set")) && called.FullName.Contains("OsuSetting")){relevant=true;break;}
    }
    if(!relevant)continue;
    ++matches;Console.WriteLine("\nMETHOD "+method.FullName);
    foreach(var i in il)Console.WriteLine($"{i.Offset:x4}: {i.OpCode} {i.Operand}");
}
if(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)))!=hash)throw new Exception("Client changed");
Console.WriteLine($"Binding methods={matches}; source hash unchanged; metadata only");
return matches>0?0:1;
