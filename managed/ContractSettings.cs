using System.Text;

// Read only the tool's own configuration. There is no game-side environment
// mutation or registry setting, and contract mode is fixed for one attachment.
public static class ContractSettings
{
    public static AudioContract.ContractMode Read(string root)
    {
        string path=Path.Combine(root,"config.ini");
        if(!File.Exists(path))return AudioContract.ContractMode.Adaptive;
        string? selected=null;
        foreach(string raw in File.ReadAllLines(path,Encoding.UTF8)) {
            string line=raw.Trim();if(line.Length==0||line[0] is '#' or ';' or '[')continue;
            int equals=line.IndexOf('=');if(equals<0||line[..equals].Trim()!="Contract Mode")continue;
            if(selected!=null)throw new InvalidDataException("Duplicate Contract Mode.");
            string value=line[(equals+1)..];int comment=value.IndexOfAny(new[]{'#',';'});if(comment>=0)value=value[..comment];selected=value.Trim();
        }
        return selected switch {null or "1"=>AudioContract.ContractMode.Adaptive,"0"=>AudioContract.ContractMode.Strict,_=>throw new InvalidDataException("Contract Mode must be 0 (Strict) or 1 (Adaptive).")};
    }
}
