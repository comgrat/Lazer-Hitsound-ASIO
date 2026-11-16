using System.Runtime.InteropServices;

// Separate per-PID attach-status mapping; shared audio queue ABI is unchanged.
public sealed class ContractSession : IDisposable
{
    [DllImport("kernel32",SetLastError=true,CharSet=CharSet.Unicode)] static extern IntPtr OpenFileMappingW(uint access,bool inherit,string name);
    [DllImport("kernel32",SetLastError=true)] static extern IntPtr MapViewOfFile(IntPtr handle,uint access,uint high,uint low,UIntPtr bytes);
    [DllImport("kernel32")] static extern bool UnmapViewOfFile(IntPtr address);
    [DllImport("kernel32")] static extern bool CloseHandle(IntPtr handle);
    IntPtr handle,view;
    ContractSession(){}
    public static ContractSession? Open(int pid)
    {
        var session=new ContractSession();
        session.handle=OpenFileMappingW(0x000f001f,false,"Local\\OsuAsioLive_"+pid);
        if(session.handle==IntPtr.Zero)return null;
        session.view=MapViewOfFile(session.handle,0x000f001f,0,0,(UIntPtr)24);
        if(session.view==IntPtr.Zero){session.Dispose();throw new InvalidDataException("Cannot map contract-session ABI.");}
        if(Marshal.ReadInt32(session.view,0)!=0x41534c32||Marshal.ReadInt32(session.view,4)!=pid){session.Dispose();throw new InvalidDataException("Incompatible contract-session ABI or PID.");}
        return session;
    }
    public AudioContract.ContractMode Mode => Marshal.ReadInt32(view,16) switch {0=>AudioContract.ContractMode.Strict,1=>AudioContract.ContractMode.Adaptive,_=>throw new InvalidDataException("Invalid frozen Contract Mode.")};
    public void Publish(AudioContract.MatchStatus status)
    {
        int value=status switch {AudioContract.MatchStatus.ExactMatch=>1,AudioContract.MatchStatus.AdaptiveMatch=>2,_=>0};
        Thread.MemoryBarrier();Marshal.WriteInt32(view,20,value);Thread.MemoryBarrier();
    }
    public void Dispose(){if(view!=IntPtr.Zero){UnmapViewOfFile(view);view=IntPtr.Zero;}if(handle!=IntPtr.Zero){CloseHandle(handle);handle=IntPtr.Zero;}}
}
