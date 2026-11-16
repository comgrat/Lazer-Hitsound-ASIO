using System.Reflection;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;

static string ClientDirectory(string input)
{
    string root=Path.GetFullPath(input);if(File.Exists(root))root=Path.GetDirectoryName(root)!;
    bool Valid(string path)=>new[]{"osu!.dll","osu.Framework.dll","osu.Game.dll"}.All(n=>File.Exists(Path.Combine(path,n)));
    if(Valid(root))return root;
    var candidates=Directory.GetDirectories(root).Where(p=>Path.GetFileName(p)=="current"||Path.GetFileName(p).StartsWith("app-",StringComparison.OrdinalIgnoreCase)).Where(Valid).ToArray();
    if(candidates.Length!=1)throw new ArgumentException("Specify the active app/current directory explicitly; candidates="+string.Join(", ",candidates.Select(Path.GetFileName)));
    return candidates[0];
}
static (Assembly Framework,Assembly Game,AssemblyLoadContext Context) Load(string directory)
{
    var context=new AssemblyLoadContext("contract:"+Guid.NewGuid(),true);
    context.Resolving+=(_,name)=>{string file=Path.Combine(directory,name.Name+".dll");return File.Exists(file)?context.LoadFromAssemblyPath(file):null;};
    return(context.LoadFromAssemblyPath(Path.Combine(directory,"osu.Framework.dll")),context.LoadFromAssemblyPath(Path.Combine(directory,"osu.Game.dll")),context);
}
static void WriteNew(string path,string text)
{
    path=Path.GetFullPath(path);Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    using var file=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None);
    using var writer=new StreamWriter(file,new UTF8Encoding(false));writer.Write(text);
}
static string Json(object value)=>JsonSerializer.Serialize(value,AudioContract.JsonOptions);
static string[] Diff(AudioContract.PolicyProfile baseline,AudioContract.PolicyProfile observed)
{
    var lines=new List<string>();
    foreach(var rule in baseline.Structure){var actual=observed.Structure.FirstOrDefault(r=>r.Id==rule.Id);if(actual==null||!actual.Signatures.SequenceEqual(rule.Signatures))lines.Add("Tier1 "+rule.Id+"\n  expected: "+string.Join("; ",rule.Signatures)+"\n  actual: "+(actual==null?"missing":string.Join("; ",actual.Signatures)));}
    foreach(var rule in baseline.Anchors){var actual=observed.Anchors.FirstOrDefault(r=>r.Id==rule.Id);if(actual==null||!actual.Instructions.SequenceEqual(rule.Instructions)){
        lines.Add("Tier2 "+rule.Id);if(actual!=null){var differing=Enumerable.Range(0,Math.Max(actual.Instructions.Length,rule.Instructions.Length)).Where(i=>i>=actual.Instructions.Length||i>=rule.Instructions.Length||actual.Instructions[i]!=rule.Instructions[i]).Take(12);foreach(int i in differing){lines.Add("  @"+i+" expected: "+(i<rule.Instructions.Length?rule.Instructions[i]:"<end>"));lines.Add("  @"+i+" actual: "+(i<actual.Instructions.Length?actual.Instructions[i]:"<end>"));}}
    }}
    return lines.ToArray();
}
try {
    Console.OutputEncoding=new UTF8Encoding(false);
    if(args.Length<3||args[0] is not ("inspect" or "append-reviewed")){
        Console.Error.WriteLine("usage: ContractTool inspect <client-dir> <candidate.json> [catalog.json]\n       ContractTool append-reviewed <client-dir> <catalog.json> <review-id> --reviewed");return 2;
    }
    string directory=ClientDirectory(args[1]);
    string output=Path.GetFullPath(args[2]);
    if(output.StartsWith(directory+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new ArgumentException("Output/catalog must be outside the client directory; clients are read-only.");
    AudioContract.Catalog catalog;
    string? catalogPath=args[0]=="append-reviewed"?output:args.Length>3?Path.GetFullPath(args[3]):null;
    if(catalogPath!=null){using var input=File.OpenRead(catalogPath);catalog=AudioContract.ReadCatalog(input);}else catalog=AudioContract.EmbeddedCatalog;
    var client=Load(directory);
    try {
        var result=AudioContract.Evaluate(client.Framework,client.Game,AudioContract.ContractMode.Adaptive,catalog);
        AudioContract.Snapshot? framework=null,game=null;
        AudioContract.PolicyProfile? observed=null;var extractionErrors=new List<string>();
        try{framework=AudioContract.Inspect(client.Framework);}catch(Exception error){extractionErrors.Add("Framework snapshot: "+error.Message);}
        try{game=AudioContract.Inspect(client.Game);}catch(Exception error){extractionErrors.Add("Game snapshot: "+error.Message);}
        try{observed=AudioContract.CapturePolicy(client.Framework,client.Game,catalog.Profiles.First(p=>p.Id==result.Policy));}catch(Exception error){extractionErrors.Add("Policy: "+error.GetType().Name+": "+error.Message);}
        if(args[0]=="inspect"){
            var candidate=new {kind="unreviewed-candidate",extractedUtc=DateTime.UtcNow,framework,game,observedPolicy=observed,extractionErrors,validation=result};
            WriteNew(output,Json(candidate));
            var report=new StringBuilder("Audio contract candidate — not promoted\n").AppendLine("Client: "+directory).AppendLine("Status: "+result.Status)
                .AppendLine("Framework: "+(framework?.Version??"unavailable")).AppendLine("Game: "+(game?.Version??"unavailable"));
            foreach(var error in result.StructuralErrors.Concat(result.SemanticErrors).Concat(result.ExactDifferences))report.AppendLine(error);
            if(observed!=null)foreach(string difference in Diff(catalog.Profiles.First(p=>p.Id==observed.Id),observed))report.AppendLine(difference);
            foreach(string error in extractionErrors)report.AppendLine("Partial extraction: "+error);
            WriteNew(output+".diff.txt",report.ToString());
            Console.WriteLine("Candidate: "+output+"\nDiff: "+output+".diff.txt\nStatus="+result.Status+"; catalog unchanged.");
            return result.Accepted?0:3;
        }
        if(args.Length!=5||args[4]!="--reviewed"||string.IsNullOrWhiteSpace(args[3]))throw new ArgumentException("Promotion requires an explicit review ID and --reviewed acknowledgement.");
        AudioContract.RequireAccepted(result);
        if(observed==null||framework==null||game==null||extractionErrors.Count>0)throw new InvalidDataException("Cannot promote a partially extracted candidate.");
        if(catalog.ReviewedContracts.Any(r=>r.Id==args[3]))throw new ArgumentException("Review ID already exists.");
        var entry=new AudioContract.ReviewedContract(args[3],result.Policy!,framework,game);
        var next=catalog with {ReviewedContracts=catalog.ReviewedContracts.Append(entry).ToArray()};
        // Never learn a new structural/semantic policy merely to accept a build.
        // Existing Tier1/Tier2 must pass, and only its exact pair is appended.
        string backup=output+".before-"+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+".json";
        File.Copy(output,backup,false);
        string temporary=output+"."+Guid.NewGuid().ToString("N")+".tmp";
        WriteNew(temporary,Json(next));File.Move(temporary,output,true);
        Console.WriteLine("Appended reviewed exact pair "+args[3]+"; policy unchanged. Backup: "+backup+"\nRebuild the managed bridge; runtime reads only the embedded catalog.");return 0;
    } finally {client.Context.Unload();}
} catch(Exception error){Console.Error.WriteLine(error);return 1;}
