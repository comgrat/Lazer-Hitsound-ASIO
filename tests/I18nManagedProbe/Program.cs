using System.Reflection;
using System.Runtime.Loader;
using System.Text;

Console.OutputEncoding=new UTF8Encoding(false);
if(args.Length!=3) return 2;
string package=Path.GetFullPath(args[0]),client=Path.GetFullPath(args[1]),evidence=Path.GetFullPath(args[2]);
Directory.CreateDirectory(evidence);
int checks=0;
void Check(bool condition,string message){checks++;if(!condition)throw new InvalidOperationException(message);}
AssemblyLoadContext.Default.Resolving+=(_,name)=>{
    string file=Path.Combine(client,name.Name+".dll");
    return File.Exists(file)?AssemblyLoadContext.Default.LoadFromAssemblyPath(file):null;
};
var framework=AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(client,"osu.Framework.dll"));
var game=AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(client,"osu.Game.dll"));
var codes=new[]{"zh-CN","zh-TW","en","fr","es"};
foreach(string code in codes){
    string directory=Path.Combine(evidence,code);
    Directory.CreateDirectory(directory);Directory.CreateDirectory(Path.Combine(directory,"locales"));
    foreach(string file in Directory.GetFiles(Path.Combine(package,"locales"),"*.lang"))File.Copy(file,Path.Combine(directory,"locales",Path.GetFileName(file)),true);
    File.WriteAllText(Path.Combine(directory,"language.ini"),"[Interface]\nLanguage = "+code+"\n",Encoding.ASCII);
    UiText.Initialize(directory);
    Check(UiText.Language==code,"language preference "+code);
    var expected=File.ReadAllLines(Path.Combine(directory,"locales",code+".lang"),Encoding.UTF8).Where(x=>x.Length>0&&x[0]!='#'&&x.Contains('=')).ToDictionary(x=>x[..x.IndexOf('=')],x=>x[(x.IndexOf('=')+1)..]);
    Check(UiText.Get("managed.ready")==expected["managed.ready"],"managed catalog "+code);
    Check(UiText.Get("watcher.found",123,"F:\\{0}\\game").Contains("F:\\{0}\\game"),"single-pass placeholders "+code);
    AudioContract.Validate(framework);AudioContract.Validate(game);
    Check(true,"unmodified audio contract "+code);
    try{AudioContract.Validate(typeof(Program).Assembly);Check(false,"invalid contract accepted");}catch(NotSupportedException){Check(true,"unknown assembly rejects without language affecting contract");}
    File.Delete(Path.Combine(directory,"locales",code+".lang"));
    UiText.Initialize(directory);
    Check(UiText.Get("managed.ready").StartsWith("Both audio adapters"),"missing catalog English fallback "+code);
}
Console.WriteLine($"I18nManagedProbe checks={checks}, failures=0; real framework/game metadata, no game/ASIO startup");
return 0;
