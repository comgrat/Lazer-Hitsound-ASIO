# Verification and limits

This record distinguishes software checks from user listening reports. Raw investigation logs and machine-specific paths are not published with the source snapshot. Version 1.1.2 keeps the binaries from the listening-tested review-round2 candidate commit `ecaa8c96dfa41c75daac36bc89fa628625a7a409`; release source/docs are tagged separately, and the package manifest records `binary_source_commit`. Earlier release results below remain identified by version.

## User listening reports

The standalone implementation was tested by its user with a normally started Game2026.1110 / Framework2026.1030 client and a TOPPING E2x2 at 48000 Hz / 16 samples. The user confirmed gameplay hitsounds at an in-game Hitsound setting of 0%, disappearance of the reported echo after asynchronous diagnostic writes, and silence with the tool-side Output Volume set to 0.

The user then tested the v1.1.1-rc.1 local candidate, reported no observed problems and subjectively felt lower latency than before, and approved its release as v1.1.1. There was no loopback recording or controlled end-to-end latency measurement for that comparison. The formal release preserves the candidate's native and managed binaries; the manifest records their binary source commit separately from the release documentation commit.

The user also tested the review-round2 candidate (`ecaa8c96dfa41c75daac36bc89fa628625a7a409`), reported no problems and approved publication as v1.1.2. The release retains all product binaries from that candidate. No new device/session details or controlled acoustic measurements were supplied for this approval.

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

Version 1.1.2 retains structural signatures, critical semantic projections and exact reviewed Game/Framework pairs. Adaptive is the default; Strict additionally requires the exact reviewed pair. Neither mode skips Tier1/2. See [AUDIO_CONTRACT.md](AUDIO_CONTRACT.md). Reviewed references remain .NET10 Game2026.1026/1110 and Framework2026.1019/1030. A failed Tier1/2 check refuses attachment in either mode. In-game Hitsound 0 can make native fallback silent.

The software buffer size is a configuration, not a latency measurement. No blanket 16-sample stability or 32/64/128 acoustic comparison claim is made.

## v1.1.0 contract regressions

The two reference client families each passed 77 assertions covering exact matching in both modes, harmless NOP/unrelated-helper drift, signature/field/critical-call/arithmetic changes, mixer creation and GameplayMixer identifier changes, interface removal, and BASS enum changes. Tests modify temporary copies only and never invoke mutated audio methods. The original assemblies remain read-only. The default build also runs synthetic tiered decisions and the frozen-mode/session ABI tests.

A separate install probe loads the real audio assemblies in isolated processes: Exact/Strict and harmless-drift/Adaptive install the expected Harmony patches; drift/Strict and a broken anchor are rejected with no patches left on the inspected targets. These tests do not launch the game or open ASIO hardware. This release has no new physical listening/latency claim.

Candidate extraction was tested to leave the embedded source catalog unchanged. Explicit append added exactly one reviewed pair, retained the structural/semantic policy and wrote a recovery copy. Structural/semantic breaks are not promoted. Runtime reads the rebuilt embedded catalog, not a mutable loose JSON.

## Round2 A1 synthetic queue wake measurement

On Windows 11 Pro 10.0.26300 x64, using MSVC 14.44.35207, Windows SDK 10.0.26100.0 and an optimized C++17 /O2 /MT build, separate process runs of `WakeLatencyProbe --old` and `--new` each exchanged 256 acknowledged events. The old consumer uses Sleep(1) without requesting a timer period; the new consumer uses an auto-reset event, a 5 ms wait timeout and timeBeginPeriod(1). The producer uses the same QPC timestamp and atomic handshake in both modes.

| Synthetic queue path | n | p50 ms | p95 ms | max ms |
| --- | ---: | ---: | ---: | ---: |
| Sleep(1), no timer request | 256 | 15.057800 | 15.798000 | 16.210900 |
| Event plus 1 ms timer request | 256 | 0.013400 | 0.018400 | 0.089200 |

These are measurements from one local producer/consumer fixture, with a producer that yields between enqueue attempts. They do not measure BASS, a real gameplay workload, ASIO hardware or acoustic latency. Scheduler load and power state can change the result. Core runs print new measurements without a timing threshold; only completion and the sample count are asserted.

Production diagnostics collect enqueue-to-handler QPC delay for PLAY events in a bounded 4096-value window and log p50/p95/max every five seconds when Debug Log=1 or ASIO_DIAGNOSTICS=1. These numbers do not establish physical latency; the user's separate listening approval is recorded above.

## v1.1.2 regression and package evidence

The final candidate build passed from an unrelated working directory with `NoDefaultCurrentDirectoryInExePath=1`, using the toolchain above and .NET SDK 10.0.404. `Build.ps1` invoked the full `Test-Core.ps1`: nine native regression programs, two wake-probe invocations and the managed contract/graph probes all passed.

| Program | Checks or result | Failures |
| --- | --- | ---: |
| Routing | 185 | 0 |
| AudioLeaseMock | 23 | 0 |
| NativeHookMock | 283 across 24 cases | 0 |
| NativeRestartProbe | 28 | 0 |
| MixerAbiRegression | 188 | 0 |
| PlayerOutputProbe | 27 | 0 |
| OutputRegression | 351 across 13 stages | 0 |
| DigitalOutputProbe | 3540 | 0 |
| ConfigEnumerationProbe | empty exit=0, injected error exit=1, deviceOpens=0, releases=2; PASS | 0 |
| WakeLatencyProbe old/new | 256 acknowledged events per mode | 0 |
| Tiered contract/config/session probe | 33 | 0 |
| ManagedGraphProbe | 11 | 0 |
| Unicode/config public boundaries | 32 | 0 |
| Portable candidate package | 136, no warnings | 0 |
| Multilingual candidate package | 106 | 0 |

New cases cover the named auto-reset wake event, idle heartbeat/burst draining and QPC collection; reordered/commented debug configuration; query passthrough under a held lock; handle release/reuse and same-generation pause/resume during unlocked monitoring; FMOD-owned PCM after source mapping mutation; actual stolen loop handles and generation-specific native restoration, unrelated-channel preservation and a full failure ring; bounded retired history, eviction replay rejection and legal new-generation playback. The failure messages printed by negative fixtures are expected assertions, not build failures.

The protocol magic changed once to `0x41534933`. The hook/backend pair must be delivered together. Retired generations, the 250 ms lease DSP, native restoration and Tier1/2 remain enforced. Full epoch recovery and one-shot natural-tail deferral are design only; see [MAINTENANCE.md](MAINTENANCE.md).

## Exact contract matching and client versions

The exact (Tier3) contract compares normalized hashes of the bridged audio implementation types, not assembly version numbers or MVIDs. A user session on Game 2026.1115 / Framework 2026.1114 matched the reviewed `game2026.1110-framework2026.1030` audio implementation and displayed `[Verified Contract]`. This shows a later build with an unchanged audio implementation; it is not a general claim about future clients.
