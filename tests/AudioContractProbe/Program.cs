using System.Reflection;
using System.Reflection.Emit;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace AudioContractProbe;

internal static class Program
{
    const string FrameworkAssembly = "osu.Framework";
    const string GameAssembly = "osu.Game";
    const string SkinnableSound = "osu.Game.Skinning.SkinnableSound";
    const string GetterName = "get_PlayWhenZeroVolume";
    const int ExpectedGetterIlBytes = 7;
    static readonly byte[] ReplacementIl = { 0x17, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00 }; // ldc.i4.1; ret; nop x5
    static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    static readonly List<CheckResult> Results = new();
    static int checks;
    static int failures;

    static void Check(bool condition, string name, string? detail = null)
    {
        checks++;
        if (!condition) failures++;
        Results.Add(new CheckResult(name, condition, detail));
        Console.WriteLine((condition ? "PASS " : "FAIL ") + name + (detail == null ? "" : " | " + detail));
    }

    static (bool Ok, string Error) TryValidate(Assembly assembly)
    {
        try
        {
            AudioContract.Validate(assembly);
            return (true, "");
        }
        catch (Exception exception)
        {
            return (false, exception.GetType().Name + ": " + exception.Message);
        }
    }

    static string Sha256(string path) =>
        Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();

    static bool SameContract(AudioContract.Snapshot left, AudioContract.Snapshot right) =>
        left.Types.Count == right.Types.Count &&
        left.Types.All(pair => right.Types.TryGetValue(pair.Key, out string? hash) && hash == pair.Value);

    static string[] ChangedTypes(AudioContract.Snapshot baseline, AudioContract.Snapshot actual) =>
        actual.Types.Where(pair => !baseline.Types.TryGetValue(pair.Key, out string? hash) || hash != pair.Value)
            .Select(pair => pair.Key).ToArray();

    static Assembly LoadBytes(AssemblyLoadContext context, string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        return context.LoadFromStream(stream);
    }

    sealed class InstallLoadContext : AssemblyLoadContext
    {
        readonly string install;
        readonly Dictionary<string, string> overrides;

        public InstallLoadContext(string install, params (string Name, string Path)[] overrides)
            : base("AudioContractProbe:" + Path.GetFileName(install), isCollectible: true)
        {
            this.install = install;
            this.overrides = overrides.ToDictionary(pair => pair.Name, pair => pair.Path, StringComparer.OrdinalIgnoreCase);
        }

        protected override Assembly? Load(AssemblyName name)
        {
            if (string.IsNullOrEmpty(name.Name)) return null;
            if (overrides.TryGetValue(name.Name, out string? overridden) && File.Exists(overridden))
                return LoadBytes(this, overridden);
            string candidate = Path.Combine(install, name.Name + ".dll");
            return File.Exists(candidate) ? LoadBytes(this, candidate) : null;
        }
    }

    static string MakeStubAssembly(string path, string assemblyName, params string[] typeNames)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var builder = new PersistedAssemblyBuilder(new AssemblyName(assemblyName), typeof(object).Assembly);
        ModuleBuilder module = builder.DefineDynamicModule(assemblyName + ".dll");
        foreach (string typeName in typeNames)
        {
            TypeBuilder type = module.DefineType(typeName, TypeAttributes.Public | TypeAttributes.Class);
            type.CreateType();
        }
        builder.Save(path);
        return path;
    }

    sealed record MethodLocation(int Rva, int FileOffset, int HeaderSize, int CodeSize, int MaxStack, byte[] Il);
    sealed record MutationEvidence(
        string SourcePath, string CopyPath, string SourceSha256Before, string CopySha256Before,
        string CopySha256After, string SourceSha256After, string MvidBefore, string MvidAfter,
        int OriginalIlBytes, string ReplacementHex, string[] ChangedContractTypes, bool Rejected, string Rejection,
        string StubLayoutPath, string StubLayoutSha256, string StubMissingTypePath, string StubMissingTypeSha256);

    static MethodLocation ReadGetterLocation(string path)
    {
        using var stream = File.OpenRead(path);
        using var pe = new PEReader(stream);
        MetadataReader metadata = pe.GetMetadataReader();
        foreach (MethodDefinitionHandle handle in metadata.MethodDefinitions)
        {
            MethodDefinition method = metadata.GetMethodDefinition(handle);
            TypeDefinition declaring = metadata.GetTypeDefinition(method.GetDeclaringType());
            if (metadata.GetString(declaring.Namespace) != "osu.Game.Skinning" ||
                metadata.GetString(declaring.Name) != "SkinnableSound" ||
                metadata.GetString(method.Name) != GetterName)
                continue;

            int rva = method.RelativeVirtualAddress;
            if (rva == 0) throw new InvalidDataException("getter 没有方法体");
            MethodBodyBlock body = pe.GetMethodBody(rva);
            byte[] il = body.GetILBytes()!;
            if (il.Length != ExpectedGetterIlBytes)
                throw new InvalidDataException($"getter IL 长度不是 {ExpectedGetterIlBytes}，实际 {il.Length}");

            int fileOffset = RvaToFileOffset(pe, rva);
            using FileStream raw = File.OpenRead(path);
            raw.Position = fileOffset;
            byte first = (byte)raw.ReadByte();
            int headerSize;
            int codeSize;
            if ((first & 3) == 2)
            {
                headerSize = 1;
                codeSize = first >> 2;
            }
            else if ((first & 3) == 3)
            {
                headerSize = 12;
                var header = new byte[12];
                raw.ReadExactly(header);
                codeSize = BitConverter.ToInt32(header, 4);
            }
            else
            {
                throw new InvalidDataException("无法识别的方法体头");
            }
            if (headerSize + codeSize != body.Size)
                throw new InvalidDataException($"方法体大小不一致：header {headerSize}+code {codeSize} != body {body.Size}");
            if (codeSize != il.Length)
                throw new InvalidDataException($"方法体代码大小不一致：{codeSize} != {il.Length}");
            if (body.ExceptionRegions.Length != 0)
                throw new InvalidDataException("getter 意外包含异常处理区");
            return new MethodLocation(rva, fileOffset, headerSize, codeSize, body.MaxStack, il);
        }
        throw new InvalidDataException($"找不到方法 {SkinnableSound}.{GetterName}");
    }

    static int RvaToFileOffset(PEReader pe, int rva)
    {
        foreach (SectionHeader section in pe.PEHeaders.SectionHeaders)
        {
            int span = Math.Max(section.VirtualSize, section.SizeOfRawData);
            if (rva >= section.VirtualAddress && rva < section.VirtualAddress + span)
                return section.PointerToRawData + (rva - section.VirtualAddress);
        }
        throw new InvalidDataException("RVA 不在任何 PE 区段内");
    }

    static Guid ReadMvid(string path)
    {
        using var stream = File.OpenRead(path);
        using var pe = new PEReader(stream);
        MetadataReader metadata = pe.GetMetadataReader();
        return metadata.GetGuid(metadata.GetModuleDefinition().Mvid);
    }

    static MutationEvidence RunMutationTest(string install, string gamePath, string work, AudioContract.Snapshot baseline)
    {
        string mutationDirectory = Path.Combine(work, "mutation");
        Directory.CreateDirectory(mutationDirectory);
        string copyPath = Path.Combine(mutationDirectory, "osu.Game.dll");
        File.Copy(gamePath, copyPath, overwrite: true);

        string sourceBefore = Sha256(gamePath);
        string copyBefore = Sha256(copyPath);
        string mvidBefore = ReadMvid(copyPath).ToString("D");
        MethodLocation location = ReadGetterLocation(copyPath);
        byte[] bytes = File.ReadAllBytes(copyPath);
        bytes.AsSpan(location.FileOffset + location.HeaderSize, ExpectedGetterIlBytes).Clear();
        ReplacementIl.CopyTo(bytes.AsSpan(location.FileOffset + location.HeaderSize));
        if (location.HeaderSize == 12 && location.MaxStack < 1)
            BitConverter.GetBytes((ushort)1).CopyTo(bytes, location.FileOffset + 2);
        File.WriteAllBytes(copyPath, bytes);

        string copyAfter = Sha256(copyPath);
        string mvidAfter = ReadMvid(copyPath).ToString("D");
        MethodLocation rewritten = ReadGetterLocation(copyPath);
        byte[] rewrittenIl = rewritten.Il;
        Check(rewrittenIl.SequenceEqual(ReplacementIl), "mutated getter body is exactly 7 bytes: ldc.i4.1; ret; nops",
            Convert.ToHexString(rewrittenIl));
        Check(mvidAfter == mvidBefore, "mutated test copy keeps its original MVID", mvidAfter);
        Check(copyAfter != copyBefore, "mutated test copy hash differs from the pre-edit copy", copyAfter);
        Check(copyBefore == sourceBefore, "pre-edit copy hash equals the read-only source hash", copyBefore);

        bool rejected;
        string error;
        string[] changed;
        var context = new InstallLoadContext(install, (GameAssembly, copyPath));
        try
        {
            _ = LoadBytes(context, Path.Combine(install, FrameworkAssembly + ".dll"));
            Assembly mutatedGame = LoadBytes(context, copyPath);
            AudioContract.Snapshot mutated = AudioContract.Inspect(mutatedGame);
            changed = ChangedTypes(baseline, mutated);
            Check(mutated.Mvid == baseline.Mvid, "mutated assembly reports the same MVID through reflection", mutated.Mvid.ToString("D"));
            Check(changed.Length == 1 && changed[0] == SkinnableSound,
                "only SkinnableSound contract hash changed after test-copy IL edit", string.Join(",", changed));
            (rejected, error) = TryValidate(mutatedGame);
        }
        finally
        {
            context.Unload();
        }
        Check(!rejected, "AudioContract.Validate rejects the IL-mutated test copy", error);
        Check(error.Contains("[Contract Failed]") && error.Contains("Tier2") && error.Contains(SkinnableSound),
            "rejection identifies the audio contract and SkinnableSound", error);
        // Deliberately do not resolve or invoke get_PlayWhenZeroVolume; reading IL does not run the getter.
        string sourceAfter = Sha256(gamePath);
        Check(sourceAfter == sourceBefore, "read-only source game DLL hash is unchanged after mutation test", sourceAfter);

        string missingPath = MakeStubAssembly(Path.Combine(work, "synthetic", "osu.Framework.dll"),
            FrameworkAssembly, "osu.Framework.Audio.AudioAdjustments");
        string missingHash = Sha256(missingPath);
        var missingContext = new InstallLoadContext(install, (FrameworkAssembly, missingPath));
        try
        {
            Assembly missing = LoadBytes(missingContext, missingPath);
            _ = LoadBytes(missingContext, gamePath);
            (bool missingAccepted, string missingError) = TryValidate(missing);
            Check(!missingAccepted && missingError.Contains("Tier1"),
                "AudioContract rejects an osu.Framework layout with a missing audio type", missingError);
        }
        finally
        {
            missingContext.Unload();
        }

        string stubPath = MakeStubAssembly(Path.Combine(work, "synthetic", "osu.Game.dll"),
            GameAssembly, SkinnableSound, "osu.Game.Skinning.PoolableSkinnableSample", "osu.Game.Rulesets.UI.DrawableRuleset");
        string stubHash = Sha256(stubPath);
        var stubContext = new InstallLoadContext(install, (GameAssembly, stubPath));
        try
        {
            _ = LoadBytes(stubContext, Path.Combine(install, FrameworkAssembly + ".dll"));
            Assembly stub = LoadBytes(stubContext, stubPath);
            (bool stubAccepted, string stubError) = TryValidate(stub);
            Check(!stubAccepted && stubError.Contains("Tier1"),
                "AudioContract rejects a synthetic osu.Game layout with no reviewed members", stubError);
        }
        finally
        {
            stubContext.Unload();
        }

        return new MutationEvidence(gamePath, copyPath, sourceBefore, copyBefore, copyAfter, sourceAfter,
            mvidBefore, mvidAfter, location.CodeSize, Convert.ToHexString(ReplacementIl), changed, !rejected, error,
            stubPath, stubHash, missingPath, missingHash);
    }

    static int Run(string[] args)
    {
        string? oldDirectory = null, newDirectory = null, work = null, summaryPath = null;
        for (int index = 0; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--old": oldDirectory = args[++index]; break;
                case "--new": newDirectory = args[++index]; break;
                case "--work": work = args[++index]; break;
                case "--summary": summaryPath = args[++index]; break;
                default: throw new ArgumentException("未知参数：" + args[index]);
            }
        }
        if (oldDirectory == null || newDirectory == null || work == null)
        {
            Console.Error.WriteLine("usage: AudioContractProbe --old <dir> --new <dir> --work <dir> [--summary <file>]");
            return 2;
        }
        oldDirectory = Path.GetFullPath(oldDirectory).TrimEnd('\\');
        newDirectory = Path.GetFullPath(newDirectory).TrimEnd('\\');
        work = Path.GetFullPath(work).TrimEnd('\\');
        summaryPath ??= Path.Combine(work, "probe-summary.json");
        foreach (string directory in new[] { oldDirectory, newDirectory })
        {
            foreach (string name in new[] { "osu!.dll", FrameworkAssembly + ".dll", GameAssembly + ".dll" })
                if (!File.Exists(Path.Combine(directory, name)))
                    throw new FileNotFoundException("缺少客户端程序集", Path.Combine(directory, name));
        }
        Directory.CreateDirectory(work);
        string oldFrameworkPath = Path.Combine(oldDirectory, FrameworkAssembly + ".dll");
        string oldGamePath = Path.Combine(oldDirectory, GameAssembly + ".dll");
        string newFrameworkPath = Path.Combine(newDirectory, FrameworkAssembly + ".dll");
        string newGamePath = Path.Combine(newDirectory, GameAssembly + ".dll");
        var sourceHashes = new Dictionary<string, string>
        {
            ["old/osu.Framework.dll"] = Sha256(oldFrameworkPath),
            ["old/osu.Game.dll"] = Sha256(oldGamePath),
            ["new/osu.Framework.dll"] = Sha256(newFrameworkPath),
            ["new/osu.Game.dll"] = Sha256(newGamePath)
        };

        AudioContract.Snapshot oldFramework, oldGame, newFramework, newGame;
        var oldContext = new InstallLoadContext(oldDirectory);
        try
        {
            Assembly frameworkAssembly = LoadBytes(oldContext, oldFrameworkPath);
            Assembly gameAssembly = LoadBytes(oldContext, oldGamePath);
            oldFramework = AudioContract.Inspect(frameworkAssembly);
            oldGame = AudioContract.Inspect(gameAssembly);
            Check(oldFramework.Types.Count == 15, "old framework exposes 15 reviewed audio types", oldFramework.Types.Count.ToString());
            Check(oldGame.Types.Count == 3, "old game exposes 3 reviewed audio types", oldGame.Types.Count.ToString());
            Check(TryValidate(frameworkAssembly).Ok, "old framework audio contract is accepted");
            Check(TryValidate(gameAssembly).Ok, "old game audio contract is accepted");
        }
        finally
        {
            oldContext.Unload();
        }
        var newContext = new InstallLoadContext(newDirectory);
        try
        {
            Assembly frameworkAssembly = LoadBytes(newContext, newFrameworkPath);
            Assembly gameAssembly = LoadBytes(newContext, newGamePath);
            newFramework = AudioContract.Inspect(frameworkAssembly);
            newGame = AudioContract.Inspect(gameAssembly);
            Check(newFramework.Types.Count == 15, "new framework exposes 15 reviewed audio types", newFramework.Types.Count.ToString());
            Check(newGame.Types.Count == 3, "new game exposes 3 reviewed audio types", newGame.Types.Count.ToString());
            Check(TryValidate(frameworkAssembly).Ok, "new framework audio contract is accepted");
            Check(TryValidate(gameAssembly).Ok, "new game audio contract is accepted");
        }
        finally
        {
            newContext.Unload();
        }

        Check(oldFramework.Version.StartsWith("2026.1019", StringComparison.Ordinal),
            "old framework version is the expected 2026.1019 line", oldFramework.Version);
        Check(newFramework.Version.StartsWith("2026.1030", StringComparison.Ordinal),
            "new framework version is the expected 2026.1030 line", newFramework.Version);
        Check(oldGame.Version.StartsWith("2026.1026", StringComparison.Ordinal),
            "old game version is the expected 2026.1026 line", oldGame.Version);
        Check(newGame.Version.StartsWith("2026.1110", StringComparison.Ordinal),
            "new game version is the expected 2026.1110 line", newGame.Version);
        Check(oldFramework.Mvid != newFramework.Mvid, "framework MVID differs between old and new current",
            oldFramework.Mvid + " -> " + newFramework.Mvid);
        Check(oldGame.Mvid != newGame.Mvid, "game MVID differs between old and new current",
            oldGame.Mvid + " -> " + newGame.Mvid);
        Check(oldFramework.Version != newFramework.Version, "framework version differs between old and new current",
            oldFramework.Version + " -> " + newFramework.Version);
        Check(oldGame.Version != newGame.Version, "game version differs between old and new current",
            oldGame.Version + " -> " + newGame.Version);
        Check(SameContract(oldFramework, newFramework), "all 14 framework contract hashes match across currents");
        Check(SameContract(oldGame, newGame), "all 3 game contract hashes match across currents");

        MutationEvidence mutation = RunMutationTest(newDirectory, newGamePath, work, newGame);
        foreach (KeyValuePair<string, string> entry in sourceHashes)
        {
            string path = entry.Key.StartsWith("old/", StringComparison.Ordinal)
                ? Path.Combine(oldDirectory, entry.Key[4..])
                : Path.Combine(newDirectory, entry.Key[4..]);
            Check(Sha256(path) == entry.Value, "read-only source hash unchanged: " + entry.Key, entry.Value);
        }

        var summary = new
        {
            probe = "AudioContractProbe",
            dotnet = Environment.Version.ToString(),
            old = new { path = oldDirectory, framework = oldFramework, game = oldGame },
            @new = new { path = newDirectory, framework = newFramework, game = newGame },
            source_sha256 = sourceHashes,
            mutation,
            checks,
            failures,
            passed = failures == 0
        };
        Directory.CreateDirectory(Path.GetDirectoryName(summaryPath)!);
        File.WriteAllText(summaryPath, JsonSerializer.Serialize(summary, JsonOptions), new UTF8Encoding(false));
        Console.WriteLine($"SUMMARY checks={checks} failures={failures}");
        return failures == 0 ? 0 : 1;
    }

    public static int Main(string[] args)
    {
        try
        {
            if(args.Length>0&&args[0]=="--tiered-self-test")return TieredRegression.Run(Array.Empty<string>());
            if(args.Length>0&&args[0]=="--tiered-client-test")return TieredRegression.Run(args.Skip(1).ToArray());
            return Run(args);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine("UNEXPECTED " + exception);
            return 3;
        }
    }

    sealed record CheckResult(string Name, bool Ok, string? Detail);
}
