using System.Collections;
using System.Reflection;
using System.Runtime.CompilerServices;

int checks=0;
void Check(bool condition,string name){checks++;if(!condition)throw new InvalidOperationException(name);Console.WriteLine("PASS "+name);}
void Gain(object root,double expected){Check(Bridge.TrySampleGain(root,out var gain,out _)&&Math.Abs(gain-expected)<1e-12,"current graph gain "+expected);}
const BindingFlags flags=BindingFlags.Static|BindingFlags.NonPublic;
object nodes=typeof(Bridge).GetField("nodes",flags)!.GetValue(null)!;
Type nodeType=typeof(Bridge).GetNestedType("VolumeNode",BindingFlags.NonPublic)!;
void Register(Graph graph){object node=Activator.CreateInstance(nodeType,BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic,null,new object[]{graph,graph.Sources},null)!;nodes.GetType().GetMethod("Add")!.Invoke(nodes,new[]{graph.Result,node});}
var graph=new Graph();var a=new Leaf{Value=0.5};var b=new Leaf{Value=0.5};
graph.Sources.Add(new Pair(a));graph.Sources.Add(new Pair(b));Register(graph);
Gain(graph.Result,0.25);a.Value=0.2;Gain(graph.Result,0.1);
lock(graph.Sources)graph.Sources.RemoveAt(1);Gain(graph.Result,0.2);
lock(graph.Sources)graph.Sources.Add(new Pair(b));Gain(graph.Result,0.1);
var nested=new Graph();nested.Sources.Add(new Pair(graph.Result));nested.Sources.Add(new Pair(b));Register(nested);Gain(nested.Result,0.05);
var reentrant=new Reentrant(()=>Bridge.TrySampleGain(graph.Result,out var gain,out _) ? gain : -1);
Gain(reentrant,0.1);Gain(nested.Result,0.05);
int failures=0;
Parallel.For(0,8,worker=>{for(int i=0;i<100;i++)if(!Bridge.TrySampleGain(nested.Result,out double gain,out _)||Math.Abs(gain-0.05)>1e-12)Interlocked.Increment(ref failures);});
Check(failures==0,"parallel graph evaluation keeps thread scratch isolated");
var weak=AddEphemeral(graph);Collect();Check(!weak.TryGetTarget(out _),"scratch clears strong source references between evaluations");
Gain(graph.Result,0.1);
for(int i=0;i<100;i++)Bridge.TrySampleGain(nested.Result,out _,out _);
long before=GC.GetAllocatedBytesForCurrentThread();
for(int i=0;i<1000;i++)Bridge.TrySampleGain(nested.Result,out _,out _);
Console.WriteLine("MEASURE graph allocated_bytes_per_call="+((GC.GetAllocatedBytesForCurrentThread()-before)/1000.0));
lock(graph.Sources)graph.Sources.Add(new Pair(graph.Result));
Check(!Bridge.TrySampleGain(graph.Result,out _,out _),"cyclic graph still rejects after scratch reuse");
Console.WriteLine("ManagedGraphProbe checks="+checks+" failures=0; no client or audio output");
return 0;

[MethodImpl(MethodImplOptions.NoInlining)] static WeakReference<object> AddEphemeral(Graph graph){object leaf=new Leaf{Value=1};var weak=new WeakReference<object>(leaf);lock(graph.Sources)graph.Sources.Add(new Pair(leaf));if(!Bridge.TrySampleGain(graph.Result,out _,out _))throw new Exception("ephemeral gain failed");return weak;}
[MethodImpl(MethodImplOptions.NoInlining)] static void Collect(){for(int i=0;i<3;i++){GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();}}
sealed class Leaf {public double Value{get;set;}}
sealed class Pair {public WeakReference<object> WeakReference{get;}public Pair(object target){WeakReference=new(target);}}
sealed class Graph {public double initialValue=1;public IList Sources{get;}=new List<object>();public object Result{get;}=new();}
sealed class Reentrant {readonly Func<double> get;public Reentrant(Func<double> get){this.get=get;}public double Value=>get();}
