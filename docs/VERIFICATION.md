# Verification and limits

This record distinguishes software checks from user listening reports. Raw investigation logs and machine-specific paths are not published with the new source snapshot.

## User listening reports

The standalone implementation was tested by its user with a normally started Game2026.1110 / Framework2026.1030 client and a TOPPING E2x2 at 48000 Hz / 16 samples. The user confirmed gameplay hitsounds at an in-game Hitsound setting of 0%, disappearance of the reported echo after asynchronous diagnostic writes, and silence with the tool-side Output Volume set to 0.

Those reports apply to the tested implementation/device/session. The later five-language UI was verified with software checks, not a new listening session in every language. They do not establish long-term stability, arbitrary skin/ruleset compatibility, account safety, or end-to-end latency.

## Software checks completed during development

| Scope | Observed result | Boundary |
| --- | --- | --- |
| Five locale catalogs | 107 keys each; 633 key/placeholder/reference checks | Text and formatting correctness |
| Public UI, language persistence, cwd/renamed path, English fallback | 100 checks, 0 failures | Real EXEs/PowerShell; isolated target and expected timeout |
| Managed localization and client metadata | 30 checks, 0 failures | No game startup or hardware output |
| Old/new real volume graphs | 42 checks per implementation, 0 failures | Explicit native stub |
| FMOD output/state regression | 228 checks, 0 failures | Real FMOD NOSOUND |
| Native restart/allocation handshake | 28 checks, 0 failures | Production detours with mock BASS |
| Digital impulse, volume and blocked diagnostic sink | 3540 checks, 0 failures | Synthetic FMOD silent-output capture |
| Portable release structure/dependencies/configuration | 130 checks, 0 failures | No game or hardware ASIO initialization |

Check counts include setup/resource assertions and render-loop assertions; they are not counts of independent real-world scenarios. The relevant fixture sources are retained under `tests/`, with a portable core runner at `tools/qa/Test-Core.ps1`.

## Echo investigation

The earlier issue was visible when different layers of a hit were started close together but synchronous diagnostic writes separated backend unpause observations. In the reported failing session, same-tick pairs had recorded spans of 46–63 ms (median47 ms). After asynchronous native/file/stdout diagnostics, 173 matched multi-layer groups had a span of 0 in the same coarse timestamp source, and the user reported no echo. `GetTickCount64` is coarse; a recorded0 does not mean physical zero latency. There was no loopback recording or strict physical causality experiment.

The implementation retains legitimate layered sounds. It does not remove events based on matching sample handles or a time window. Ended/retired playback generations cannot be revived by stale events, while explicit native replays obtain a new playback generation.

## Compatibility policy

The bridge accepts reviewed audio implementation layouts and normalized method contracts, rather than trusting only a displayed game version. Reviewed references are .NET10 Game2026.1026/1110 and Framework2026.1019/1030. Unknown implementations are rejected in favor of native output. If the in-game Hitsound setting is0, native fallback may itself be silent.

The software buffer size is a configuration, not a latency measurement. No blanket 16-sample stability or 32/64/128 acoustic comparison claim is made.
