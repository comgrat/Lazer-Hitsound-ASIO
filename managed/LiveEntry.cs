using System.Runtime.InteropServices;

// Called via the documented hostfxr component API inside an existing runtime.
public static class LiveEntry
{
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
            Bridge.LogWriter = TextWriter.Synchronized(new StreamWriter(
                Path.Combine(root, "logs", $"managed-{Environment.ProcessId}.log"), append: true) { AutoFlush = true });
            Bridge.Log("[ASIO attach] "+UiText.Get("managed.attaching",Environment.ProcessId));
            return Bridge.InstallLive() ? 0 : 3;
        }
        catch (Exception exception)
        {
            Bridge.Log("[ASIO attach] "+UiText.Get("managed.attach_error",exception.GetType().Name,exception.Message));
            return 4;
        }
    }
}
