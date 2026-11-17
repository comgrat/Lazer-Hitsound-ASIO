using System.Reflection;
using System.Reflection.Emit;
using System.Text.RegularExpressions;

public static partial class AudioContract
{
    // Policy revision 2: whole-method control/data flow remains exact except an
    // explicitly recognized, isolated Console.WriteLine(string literal) slice.
    // Generated ordinal mappings are established by unique anchored bodies and
    // reused across every caller; they are never assigned by ordinal rank.
    static readonly Regex generatedOrdinal = new(@"(?:<>c__DisplayClass|<>9__|<[^<>]+>(?:b__|d__|g__[^|<>]*\|))\d+(?:_\d+)?",RegexOptions.CultureInvariant);
    static string GeneratedFamily(string name) => generatedOrdinal.Replace(name,m=>Regex.Replace(m.Value,@"\d+(?:_\d+)?$","#"));
    static bool Generated(string name) => generatedOrdinal.IsMatch(name);
    static string WithoutOrdinals(string line) => line.StartsWith("ldstr ",StringComparison.Ordinal)?line:GeneratedFamily(line);

    sealed class ProjectionContext
    {
        readonly Dictionary<string,string> rename=new(StringComparer.Ordinal);
        readonly Dictionary<string,string> reverse=new(StringComparer.Ordinal);
        readonly Dictionary<string,MethodBase> resolved=new(StringComparer.Ordinal);
        public ProjectionContext(Assembly[] audio,PolicyProfile profile)
        {
            foreach(var rule in profile.Anchors.Where(r=>r.Method.StartsWith("prefix:",StringComparison.Ordinal)||Generated(r.Method)||Generated(r.Type))) {
                var assembly=FindAssembly(audio,rule.Assembly);
                string stored=rule.Method.StartsWith("prefix:",StringComparison.Ordinal)?rule.Method[7..]:rule.Method;
                IEnumerable<Type> types;
                if(Generated(rule.Type)) {
                    int split=rule.Type.LastIndexOf('+');
                    if(split<0)throw new NotSupportedException("Generated type has no declaring context: "+rule.Id);
                    var parent=FindType(assembly,rule.Type[..split]);
                    string family=GeneratedFamily(rule.Type[(split+1)..]);
                    types=parent.GetNestedTypes(declared).Where(t=>GeneratedFamily(t.Name)==family);
                } else types=new[]{FindType(assembly,rule.Type)};
                var candidates=types.SelectMany(t=>t.GetMethods(declared)).Where(m=>rule.Method.StartsWith("prefix:",StringComparison.Ordinal)?m.Name.StartsWith(stored,StringComparison.Ordinal):GeneratedFamily(m.Name)==GeneratedFamily(stored))
                    .Where(m=>ProjectionShapeMatches(rule.Instructions,Actual(rule,m))).ToArray();
                // Body, signature, captured field reads/writes and branches all
                // participate in this match. Ambiguous identical helpers reject.
                if(candidates.Length!=1)throw new NotSupportedException("Expected one generated anchor: "+rule.Id+"; matches="+candidates.Length);
                var target=candidates[0];resolved.Add(rule.Id,target);
                var actual=Actual(rule,target);
                Record(rule.Assembly+":"+rule.Type,rule.Instructions,actual);
            }
        }
        static string[] Actual(AnchorRule rule,MethodBase method) => SemanticInstructions(method,2);
        static bool ProjectionShapeMatches(string[] expected,string[] actual) => expected.Length==actual.Length &&
            expected.Zip(actual).All(pair=>WithoutOrdinals(pair.First)==WithoutOrdinals(pair.Second));
        void Record(string scope,string[] expected,string[] actual)
        {
            // Scope is the top-level declaring type. This ties generated anchor
            // identities to all references in the same caller/type context.
            scope=Scope(scope);
            for(int i=0;i<expected.Length;++i) {
                if(expected[i].StartsWith("ldstr ",StringComparison.Ordinal))continue;
                var left=generatedOrdinal.Matches(expected[i]);var right=generatedOrdinal.Matches(actual[i]);
                if(left.Count!=right.Count)throw new NotSupportedException("Generated operand shape differs");
                for(int j=0;j<left.Count;++j) {
                    string key=scope+"|"+left[j].Value,value=scope+"|"+right[j].Value;
                    if(rename.TryGetValue(key,out var prior)&&prior!=right[j].Value)throw new NotSupportedException("Inconsistent generated reference: "+key);
                    if(reverse.TryGetValue(value,out var other)&&other!=left[j].Value)throw new NotSupportedException("Generated identities merged: "+value);
                    rename[key]=right[j].Value;reverse[value]=left[j].Value;
                }
            }
        }
        static string Scope(string scope){int nested=scope.IndexOf('+');return nested<0?scope:scope[..nested];}
        public MethodBase Resolve(Assembly assembly,AnchorRule rule) => resolved.TryGetValue(rule.Id,out var method)?method:ResolveAnchor(assembly,rule);
        public bool Matches(AnchorRule rule,string[] actual)
        {
            if(!ProjectionShapeMatches(rule.Instructions,actual))return false;
            // Extend the same bijection to cache fields at exact instruction
            // positions. Pre-resolved method identities cannot be swapped here.
            Record(rule.Assembly+":"+rule.Type,rule.Instructions,actual);
            string scope=Scope(rule.Assembly+":"+rule.Type);
            for(int i=0;i<actual.Length;++i) {
                string expected=rule.Instructions[i];
                if(!expected.StartsWith("ldstr ",StringComparison.Ordinal))
                    expected=generatedOrdinal.Replace(expected,m=>rename.TryGetValue(scope+"|"+m.Value,out var mapped)?mapped:m.Value);
                if(expected!=actual[i])return false;
            }
            return true;
        }
    }

    // This deliberately small semantic slice proves an empty incoming stack,
    // literal-only argument, unused void result, and no control/EH entry into the
    // removed pair. All other statements and every branch remain in Tier2.
    static IlInstruction[] SliceDiagnostics(MethodBase method,IReadOnlyList<IlInstruction> il)
    {
        string type=method.DeclaringType?.FullName??"";
        bool eligible=(type=="osu.Framework.Audio.AudioManager" && method.IsConstructor) ||
            (type=="osu.Framework.Audio.Mixing.Bass.BassAudioMixer" && method.Name=="createMixer") ||
            (type=="osu.Game.Rulesets.UI.DrawableRuleset`1" && (method.IsConstructor||method.Name=="load"));
        var meaningful=il.Where(i=>i.OpCode!=OpCodes.Nop).ToArray();
        if(!eligible)return meaningful;
        var targets=new HashSet<int>();
        foreach(var instruction in il) {
            if(instruction.OpCode.OperandType is OperandType.InlineBrTarget or OperandType.ShortInlineBrTarget)targets.Add((int)instruction.Operand!);
            if(instruction.Operand is int[] jumps)foreach(int jump in jumps)targets.Add(jump);
        }
        foreach(var clause in method.GetMethodBody()!.ExceptionHandlingClauses) {
            targets.UnionWith(new[]{clause.TryOffset,clause.TryOffset+clause.TryLength,clause.HandlerOffset,clause.HandlerOffset+clause.HandlerLength});
            if(clause.Flags==ExceptionHandlingClauseOptions.Filter)targets.Add(clause.FilterOffset);
        }
        var keep=new List<IlInstruction>();
        for(int i=0;i<meaningful.Length;++i) {
            // Conservatively accept method-entry diagnostics only. This avoids
            // guessing the stack depth after calls, prefixes or local stores.
            bool boundary=i==0;
            if(boundary && i+1<meaningful.Length && meaningful[i].OpCode==OpCodes.Ldstr &&
               meaningful[i+1].OpCode==OpCodes.Call && meaningful[i+1].Operand is MethodInfo called &&
               called.DeclaringType==typeof(Console) && called.Name==nameof(Console.WriteLine) && called.ReturnType==typeof(void) &&
               called.GetParameters() is var p && p.Length==1 && p[0].ParameterType==typeof(string) &&
               !targets.Any(t=>t>=meaningful[i].Offset && t<=meaningful[i+1].Offset)) {i++;continue;}
            keep.Add(meaningful[i]);
        }
        return keep.ToArray();
    }
}
