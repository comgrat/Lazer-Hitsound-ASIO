# Build on Windows x64

Use an **x64 Native Tools Command Prompt / Developer PowerShell for Visual Studio**. Requirements:

- MSVC with C++17 support and a Windows SDK.
- .NET **10.0.404** SDK, pinned by `global.json` (`dotnet --list-sdks`).
- The x64 FMOD import library at `lib/fmod_vc.lib` and matching runtime `fmod.dll`.
- The x64 MinHook library at `lib/libMinHook.x64.lib`.

The native SDK/runtime files inherited from upstream remain in the repository. Their presence does not replace the third-party terms in [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md). Harmony 2.4.2 is restored from NuGet.

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1
```

This builds the native configuration tool, launcher, backend and hook; the .NET bridge; and the optional stereo device checker. It also runs nine native regression executables and the managed tiered-contract self-test. Each test process has a 120-second deadline and saved stdout/stderr; pass `-TestTimeoutSeconds` to `Test-Core.ps1` to adjust it. Core tests use mock/native silent output and do not launch the game or open ASIO hardware. Use `-SkipTests` to build only.

Export an untouched portable folder and ZIP:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\distribution\Build-Share.ps1 -OutputDirectory .\artifacts\LazerASIO-Tachyon1110
```

Before creating the output directory, the exporter validates the source commit and every package input. In a Git checkout it reads HEAD; for a GitHub source archive, supply its real full 40-character commit with `-SourceCommit` or `ASIO_SOURCE_COMMIT`. A missing/invalid commit fails without leaving a partial output directory. The package README comes from `docs/USAGE.md`.

The exporter resets the original five audio fields plus `Contract Mode = 1` (Adaptive) to generic defaults, omits language/device preferences and logs, preserves UTF-8 BOM for Windows PowerShell scripts, and writes file hashes. Use a new directory if the target contains runtime data. Do not package your used tool folder.

Optional package checks:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\qa\Test-SharePackage.ps1 -PackageDirectory .\artifacts\LazerASIO-Tachyon1110
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\qa\Test-Multilingual.ps1 -PackageDirectory .\artifacts\LazerASIO-Tachyon1110
```

`Test-SharePackage` requires `dumpbin.exe` from the developer shell. Device-list checks enumerate drivers but do not initialize an ASIO device. Language/launcher checks use an isolated temporary copy, a unique watcher mutex, a nonexistent process name and an expected short timeout. They do not attach to a real client.

If editing translations, keep key names and `{0}` placeholders unchanged, then run:

```powershell
node .\tools\i18n\Generate-English.cjs
node .\tools\i18n\Check-Catalogs.cjs
```

GitHub Actions runs the native/.NET build, translation validation, Unicode/config regression, portable package checks and five-language UI on Windows without a game install or ASIO hardware. Empty ASIO enumeration is valid; enumeration errors still fail. CI has a job deadline in addition to per-test timeouts.

MSVC release compilation uses `/Brepro`; the .NET SDK is pinned. This improves build traceability but does not establish byte-identical builds across toolchains, SDKs, dependency restores or ZIP timestamps. Published SHA-256 values identify delivered files; they are integrity checks, not a reproducible-build claim.

Node is only needed to regenerate/check localization sources. The installed application does not require Node or a separately installed .NET SDK.

For the real audio-graph metadata fixtures, pass an explicit compatible client directory; no account or beatmap files are needed:

```powershell
dotnet run --project .\tests\I18nManagedProbe -c Release -- .\artifacts\LazerASIO-Tachyon1110 C:\path\to\compatible-client .\tests\bin\language-evidence
dotnet run --project .\tests\LiveGraphResearch -c Release -- C:\path\to\compatible-client .\tests\bin\graph-result.json
```

These fixtures load the game's audio assemblies and create real volume graphs without starting the game. They cannot establish audible correctness or account safety.

## Maintain audio compatibility

See [AUDIO_CONTRACT.md](AUDIO_CONTRACT.md) for the schema, mode boundaries, candidate extraction and explicit reviewed-pair promotion. The runtime uses an embedded catalog; rebuild the managed bridge after updating it. The metadata-only mutation fixture uses Mono.Cecil 0.11.6 from NuGet, a test-only dependency not shipped in the portable ZIP.

```powershell
.\tools\contract\Update-AudioContract.ps1 -ClientDirectory C:\path\to\client
dotnet run --project .\tests\AudioContractProbe -c Release -- --tiered-client-test C:\path\to\client .\artifacts\contract-mutations
```
