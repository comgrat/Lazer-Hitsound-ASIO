using System.Runtime.InteropServices;

// Called via the documented hostfxr component API inside an existing runtime.
public static class LiveEntry
{
    // StreamWriter emits the preamble only for an empty append stream. Existing
    // logs, including older BOM-less UTF-8 logs, keep every byte unchanged.
    internal static TextWriter OpenDiagnosticLog(string path) => TextWriter.Synchronized(
        new StreamWriter(path, append: true, encoding: new System.Text.UTF8Encoding(true)) { AutoFlush = true });

    public static int Initialize(IntPtr argument, int bytes)
    {
        try
        {
            if (argument == IntPtr.Zero || bytes < 2 || (bytes & 1) != 0) return 2;
            string root = Marshal.PtrToStringUni(argument, bytes / 2)!.TrimEnd('\0');
            root = Path.GetFullPath(root);
            UiText.Initialize(root);
            Directory.CreateDirectory(Path.Combine(root, "logs"));
            Bridge.detailedPlayback = File.Exists(Path.Combine(root,"playback-trace.enabled"));
            Bridge.LogWriter = OpenDiagnosticLog(Path.Combine(root, "logs", $"managed-{Environment.ProcessId}.log"));
            using var session=ContractSession.Open(Environment.ProcessId);
            Bridge.Mode=session?.Mode??ContractSettings.Read(root);
            Bridge.Log("[ASIO attach] "+UiText.Get("managed.attaching",Environment.ProcessId));
            bool installed=Bridge.InstallLive();
            session?.Publish(installed?Bridge.ContractStatus:AudioContract.MatchStatus.Failed);
            return installed ? 0 : 3;
        }
        catch (Exception exception)
        {
            Bridge.Log("[ASIO attach] "+UiText.Get("managed.attach_error",exception.GetType().Name,exception.Message));
            return 4;
        }
    }
}
