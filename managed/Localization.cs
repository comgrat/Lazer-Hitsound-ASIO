using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;

// Read only the tool-side preference and catalogs. Never read/write game settings.
public static class UiText
{
    static readonly string[] supported={"zh-CN","zh-TW","en","fr","es"};
    static Dictionary<string,string> catalog=LoadEmbeddedEnglish();
    public static string Language { get; private set; }="en";
    static Dictionary<string,string> Parse(string text)
    {
        var result=new Dictionary<string,string>(StringComparer.Ordinal);
        foreach(string raw in text.Split('\n')) {
            string line=raw.TrimEnd('\r');
            if(line.Length==0||line[0]=='#')continue;
            int equals=line.IndexOf('=');
            if(equals>0)result.TryAdd(line[..equals].Trim(),line[(equals+1)..]);
        }
        return result;
    }
    static Dictionary<string,string> LoadEmbeddedEnglish()
    {
        using Stream? stream=typeof(UiText).Assembly.GetManifestResourceStream("Asio.English.lang");
        if(stream==null)return new(StringComparer.Ordinal);
        using var reader=new StreamReader(stream,Encoding.UTF8);
        return Parse(reader.ReadToEnd());
    }
    public static void Initialize(string? root=null)
    {
        try {
            root??=Path.GetDirectoryName(Path.GetDirectoryName(typeof(UiText).Assembly.Location))!;
            Language="en";
            string preference=Path.Combine(root,"language.ini");
            if(File.Exists(preference)) {
                var saved=Parse(File.ReadAllText(preference,Encoding.UTF8));
                if(saved.TryGetValue("Language",out string? code)&&supported.Contains(code.Trim()))Language=code.Trim();
            }
            var next=LoadEmbeddedEnglish();
            foreach(string code in new[]{"en",Language}.Distinct()) {
                string file=Path.Combine(root,"locales",code+".lang");
                if(File.Exists(file))foreach(var entry in Parse(File.ReadAllText(file,Encoding.UTF8)))if(next.ContainsKey(entry.Key))next[entry.Key]=entry.Value;
            }
            catalog=next;
        } catch { catalog=LoadEmbeddedEnglish();Language="en"; }
    }
    public static string Get(string key,params object?[] values)
    {
        string template=catalog.TryGetValue(key,out string? value)?value:key;
        return Regex.Replace(template,@"\{(\d+)\}",match=>int.TryParse(match.Groups[1].Value,out int n)&&n<values.Length?Convert.ToString(values[n],System.Globalization.CultureInfo.InvariantCulture)??"":match.Value);
    }
}
