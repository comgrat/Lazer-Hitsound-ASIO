using System.Reflection;
using System.Text;

var open=typeof(LiveEntry).GetMethod("OpenDiagnosticLog",BindingFlags.NonPublic|BindingFlags.Static)!;
string root=Path.Combine(Path.GetTempPath(),"AsioManagedLog_"+Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
byte[] bom={0xef,0xbb,0xbf};
int checks=0;
void Check(bool value,string label){++checks;if(!value)throw new InvalidOperationException(label);Console.WriteLine("PASS "+label);}
foreach(int mode in new[]{0,1,2}) {
    string path=Path.Combine(root,mode+".log");
    byte[] initial=mode==0?Array.Empty<byte>():mode==1?bom.Concat(Encoding.UTF8.GetBytes("已有内容\n")).ToArray():Encoding.UTF8.GetBytes("旧UTF-8日志\n");
    if(mode!=0)File.WriteAllBytes(path,initial);
    foreach(string text in new[]{"第一条中文日志","second append"}) {
        using var writer=(TextWriter)open.Invoke(null,new object[]{path})!;
        writer.WriteLine(text);
    }
    byte[] actual=File.ReadAllBytes(path);
    byte[] expected=(mode==0?bom:initial).Concat(Encoding.UTF8.GetBytes("第一条中文日志"+Environment.NewLine+"second append"+Environment.NewLine)).ToArray();
    Check(actual.SequenceEqual(expected),"new/reopened/legacy file exact UTF-8 bytes mode="+mode);
    Check(Encoding.UTF8.GetString(actual).Contains("第一条中文日志"),"Chinese text round trips mode="+mode);
}
Console.WriteLine($"ManagedLogProbe checks={checks} failures=0");
