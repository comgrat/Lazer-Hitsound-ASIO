# Verification and limits

This record distinguishes software checks from user listening reports. Raw investigation logs and machine-specific paths are not published with the new source snapshot. Version 1.1.1 keeps the binaries from the user-tested candidate commit `7792f7eaa94bd0cf09078c86748a2ff18d0c0bd7`; release source/docs are tagged separately, and the package manifest records `binary_source_commit`.

## User listening reports

The standalone implementation was tested by its user with a normally started Game2026.1110 / Framework2026.1030 client and a TOPPING E2x2 at 48000 Hz / 16 samples. The user confirmed gameplay hitsounds at an in-game Hitsound setting of 0%, disappearance of the reported echo after asynchronous diagnostic writes, and silence with the tool-side Output Volume set to 0.

The user then tested the v1.1.1-rc.1 local candidate, reported no observed problems and subjectively felt lower latency than before, and approved its release as v1.1.1. There was no loopback recording or controlled end-to-end latency measurement for that comparison. The formal release preserves the candidate's native and managed binaries; the manifest records their binary source commit separately from the release documentation commit.

Those reports apply to the tested implementation/device/session. The five-language UI was verified with software checks, not a separate listening session in every language. They do not establish long-term stability, arbitrary skin/ruleset compatibility, account safety, or end-to-end latency.

## Software checks completed during development

| Scope | Observed result | Boundary |
| --- | --- | --- |
| Five locale catalogs | 119 keys each; 702 key/placeholder/reference checks | Text and formatting correctness |
| Public UI, language persistence, cwd/renamed path, English fallback | 106 checks, 0 failures | Real EXEs/PowerShell; isolated target and expected timeout |
| Managed localization and client metadata | 30 checks, 0 failures | No game startup or hardware output |
| Old/new real volume graphs | 43 checks per implementation, 0 failures | Explicit native stub |
| FMOD output/state regression | 232 checks, 0 failures | Real FMOD NOSOUND; includes legacy/Strict/invalid mode parsing |
| Native restart/allocation handshake | 28 checks, 0 failures | Production detours with mock BASS |
| Digital impulse, volume and blocked diagnostic sink | 3540 checks, 0 failures | Synthetic FMOD silent-output capture |
| Portable release structure/dependencies/configuration | 136 checks, 0 failures | No game or hardware ASIO initialization |
| Tiered synthetic/configuration/session ABI | 33 checks, 0 failures | Production evaluator and per-PID mode/status mapping |
| Managed graph cache and traversal buffers | 11 checks, 0 failures | Mutable sources, re-entry, parallel calls, weak collection and cycles |
| Native hook/ACK regression | 215 checks, 0 failures | Cached ACK slots reject generation/handle reuse; mock BASS |
| Real-client contract mutations (v1.1.0 baseline) | 77 checks per reference family, 0 failures | Metadata/IL only; source assemblies unchanged |
| Unicode executable paths and invalid configuration | 32 cases, 0 failures | Japanese/emoji/spaces; six invalid inputs in five languages |
| Empty ASIO enumeration | empty=0, enumeration error=1, device initializations=0 | Real config entry with deterministic FMOD stubs |
| Export preflight and archive provenance | Passed | Missing commit/input leaves no output; environment fallback and checksums verified |

Version 1.1.1 adds ACK slot and finished-channel cleanup assertions plus an 11-check managed graph probe. On the same synthetic graph/runtime with warmup, allocation measured 832 bytes/call with the prior bridge versus 200 bytes/call with cached metadata/traversal buffers. This measurement is limited to that fixture; it is not a game or acoustic latency result. The user approved the corresponding rc.1 candidate after listening tests; the formal package keeps those tested binaries.

The maintenance checks add a ninth native regression and a throwing UI graph fixture. The original v1.1.0 contract mutation results remain identified as that release's baseline; this change does not alter its 80/41 contract catalog. No new hardware listening measurement was performed.

Check counts include setup/resource assertions and render-loop assertions; they are not counts of independent real-world scenarios. The relevant fixture sources are retained under `tests/`, with a portable core runner at `tools/qa/Test-Core.ps1`.

## Echo investigation

The earlier issue was visible when different layers of a hit were started close together but synchronous diagnostic writes separated backend unpause observations. In the reported failing session, same-tick pairs had recorded spans of 46–63 ms (median47 ms). After asynchronous native/file/stdout diagnostics, 173 matched multi-layer groups had a span of 0 in the same coarse timestamp source, and the user reported no echo. `GetTickCount64` is coarse; a recorded0 does not mean physical zero latency. There was no loopback recording or strict physical causality experiment.

The implementation retains legitimate layered sounds. It does not remove events based on matching sample handles or a time window. Ended/retired playback generations cannot be revived by stale events, while explicit native replays obtain a new playback generation.

## Compatibility policy

Version 1.1.1 retains structural signatures, critical semantic projections and exact reviewed Game/Framework pairs. Adaptive is the default; Strict additionally requires the exact reviewed pair. Neither mode skips Tier1/2. See [AUDIO_CONTRACT.md](AUDIO_CONTRACT.md). Reviewed references remain .NET10 Game2026.1026/1110 and Framework2026.1019/1030. A failed Tier1/2 check refuses attachment in either mode. In-game Hitsound 0 can make native fallback silent.

The software buffer size is a configuration, not a latency measurement. No blanket 16-sample stability or 32/64/128 acoustic comparison claim is made.

## v1.1.0 contract regressions

The two reference client families each passed 77 assertions covering exact matching in both modes, harmless NOP/unrelated-helper drift, signature/field/critical-call/arithmetic changes, mixer creation and GameplayMixer identifier changes, interface removal, and BASS enum changes. Tests modify temporary copies only and never invoke mutated audio methods. The original assemblies remain read-only. The default build also runs synthetic tiered decisions and the frozen-mode/session ABI tests.

A separate install probe loads the real audio assemblies in isolated processes: Exact/Strict and harmless-drift/Adaptive install the expected Harmony patches; drift/Strict and a broken anchor are rejected with no patches left on the inspected targets. These tests do not launch the game or open ASIO hardware. This release has no new physical listening/latency claim.

Candidate extraction was tested to leave the embedded source catalog unchanged. Explicit append added exactly one reviewed pair, retained the structural/semantic policy and wrote a recovery copy. Structural/semantic breaks are not promoted. Runtime reads the rebuilt embedded catalog, not a mutable loose JSON.
