using System.Reflection;
using System.Runtime.Loader;
using HarmonyLib;

Console.OutputEncoding=new System.Text.UTF8Encoding(false);
if(args.Length!=5)return 2;
string client=Path.GetFullPath(args[0]),frameworkPath=Path.GetFullPath(args[1]),gamePath=Path.GetFullPath(args[2]);
Bridge.Mode=args[3]=="strict"?AudioContract.ContractMode.Strict:AudioContract.ContractMode.Adaptive;
bool expected=args[4]=="accept";
AssemblyLoadContext.Default.Resolving+=(_,name)=>{string file=Path.Combine(client,name.Name+".dll");return File.Exists(file)?AssemblyLoadContext.Default.LoadFromAssemblyPath(file):null;};
var framework=AssemblyLoadContext.Default.LoadFromAssemblyPath(frameworkPath);
var game=AssemblyLoadContext.Default.LoadFromAssemblyPath(gamePath);
var messages=new StringWriter();Bridge.LogWriter=messages;
bool accepted=false;Exception? failure=null;
try{accepted=Bridge.InstallLive();}catch(Exception error){failure=error;}
var flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance|BindingFlags.Static;
var getter=game.GetType("osu.Game.Skinning.SkinnableSound",true)!.GetProperty("PlayWhenZeroVolume",flags)!.GetMethod!;
var state=framework.GetType("osu.Framework.Audio.Sample.SampleChannelBass",true)!.GetMethod("OnStateChanged",flags)!;
bool Patched(MethodBase method)=>Harmony.GetPatchInfo(method)?.Owners.Contains("osu.asio.offline.volume.v2")==true;
Console.Write(messages.ToString());
if(failure!=null)Console.WriteLine("EXPECTED_OR_UNEXPECTED_EXCEPTION "+failure.GetType().Name+": "+failure.Message);
bool patched=Patched(getter)||Patched(state);
if(accepted!=expected||patched!=expected){Console.Error.WriteLine($"FAIL install accepted={accepted}, anyPatch={patched}, expected={expected}");return 1;}
if(expected&&(!Patched(getter)||!Patched(state)))return 1;
if(!expected&&!messages.ToString().Contains("[Contract Failed]"))return 1;
if(expected&&Bridge.ContractStatus==AudioContract.MatchStatus.AdaptiveMatch&&!messages.ToString().Contains("[Compat/Adaptive]"))return 1;
Console.WriteLine($"ContractInstallProbe PASS mode={Bridge.Mode} accepted={accepted} patchPresence={patched} contract={Bridge.ContractStatus}; no game/audio methods invoked");
return 0;
