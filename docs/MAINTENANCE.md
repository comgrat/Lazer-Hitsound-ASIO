# Review decisions and maintenance plan

The community review of v1.1.0 identified real portability, testing and maintenance costs. This record distinguishes implemented fixes from changes that need a protocol design or external authorization.

## Round5 (local candidate, not hardware validated)

### Implemented and checked in software

- **P0 — confirmed:** v1.2.0 enabled the lease before `DllInject` synchronously waited up to 10 seconds without refreshing the heartbeat. Initialization and managed/native readiness need `backendReady`, so simply delaying readiness would break sample publication. `StartupLease` now owns a dedicated 10 ms heartbeat from mapping creation through injection/managed initialization, stops before event-worker startup and is stopped before every unmap. It cannot renew runtime playback after handoff. The non-live 1000 ms sleeps precede pool/lease creation. `StartupLeaseProbe` uses the production DSP and real FMOD NOSOUND: 50/200/300/600 ms startup waits leave Running/recoveries=0; a post-handoff 320 ms stall still latches recovery.
- **P1 — confirmed missing diagnostics:** the CAS winner first claims silent Recording, writes source/stale heartbeat/age/trigger epoch/tick using Interlocked, then publishes Requested. Other contenders cannot overwrite the cause; the backend waits while Recording. DSP work stays allocation/lock/I/O-free. Sources distinguish DSP, backend, hook and monitor's own overdue heartbeat. Completion and periodic status expose the cause. Every AsyncTextLog logical line now starts with `[t=<boot-relative milliseconds>]`, sampled in enqueue order under its existing nonblocking queue lock, so hook/backend timestamps align. Shared layout magic is **0x41534936**; replace hook and backend together.
- **P2 — diagnostics only:** Debug Log enables outermost stateLock wait/hold maxima and the longest original BASS call under that lock, reported in microseconds. Nested entries do not reset the outer clock. A deliberately blocked ChannelPlay measured about 290 ms hold/wait and identified ChannelPlay correctly. Debug-off fixtures leave counters zero. This proves the coupling mechanism under fault injection; it is not a measurement of user gameplay contention. Lock/heartbeat semantics are unchanged.
- **P4 — partly already protected:** read-only IL inspection of Game2026.1115 found `OsuSetting.HitsoundVolume=121` -> DrawableRuleset<T>.hitsoundVolume -> gameplayMixer.Volume.BindTo. That load method and its full binding sequence were already in Tier2. The bridge's GameplayMixer marker returns 1 for this user-volume source in Evaluate, preserving sample/chart factors. Round5 adds a Tier1 enum-literal rule (81 total, existing 41 anchors/reviewed hashes unchanged) and negative mutations for changing either enum value or bound setting. This is metadata evidence, not a new hardware slider test. Five-language usage now states the checked scope and silent-native-fallback limit.
- **P5 — confirmed:** TrySampleGain and per-channel reflection/ancestor failures could invoke global Disable. They now reject only the matching channel/generation with eligible=0. Other channels and backend readiness survive. Low-level observation helpers propagate local faults to their channel boundary. Shared master/mixer setup, contract validation and patch installation failures remain global. Graph/isolation probes inject throwing getters, cycles and bad parents, then verify a healthy channel still receives eligible=1 and no zero-handle shutdown was sent. Explicit global failure still sends shutdown. Token replacement uses ConditionalWeakTable.AddOrUpdate.

### Reviewed without changing behavior

**P3:** [upstream dbdbed6](https://github.com/Mother-Ship/osu-lazer-External-ASIO-Sound/commit/dbdbed60085ad0cfeaaec711da6d105c126a3b10) was read directly. PAUSE=0x20000 is already present and tested. This fork derives starts from hooks, uses polling only for completion and tests decode-active ambiguity. Muting every forwarded sample differs from GameplayMixer-only ownership and native fallback here, so it is not adopted. Timestamp observability is implemented independently. No upstream merge or ours merge was performed.

**P6:** natural one-shot completion can legitimately leave late monitor PAUSE/update/REMOVED events that the retired-generation filter rejects. staleRejected is a guard counter, not a count of lost hits. Tail deferral still needs a natural-end/explicit-stop distinction and remains design only. Source-file splitting, removal of legacy logging flags and PCM8 representation conversion are deferred; no unverified format conversion or ownership change is included in this candidate.

### Design and user validation still pending

Cold-start logs on the user's device must confirm no startup recovery; if recovery occurs, inspect its first-source/age evidence. Actual stateLock maxima require Debug Log gameplay logs. A future design could separate monitor scheduling from lock-held producer stalls, but must keep producer liveness meaningful and cannot merely hide a blocked owner with an unrelated heartbeat. The six-case Hitsound/Effect listening matrix is in [ROUND5_LISTENING.md](ROUND5_LISTENING.md). No Round5 hardware listening result is claimed. This branch is local only and is not authorized for push/tag/release.

## Implemented

- Backend/config bootstrap uses the same Windows Unicode executable-directory helper and `SetCurrentDirectoryW`. Configuration reads already used `CreateFileW`; regression runs from Japanese/emoji/spaced directories with a different working directory. This is not a claim of unlimited Windows path-length support.
- Successful `config.exe --list` with zero ASIO drivers exits 0, matching the device checker. Real enumeration failures remain nonzero. A deterministic stub checks both cases and verifies no device initialization occurs.
- Eight parser/file-read error causes use all five language catalogs. Tests exercise six invalid-input cases in every language without rewriting the files.
- Non-GameplayMixer channel callbacks emit ineligible context without owner recovery or volume-graph evaluation. A throwing graph verifies that this path is skipped. Gameplay gain and zero-volume behavior are still tested with real client assemblies and native stubs.
- Windows CI builds and checks the software without a client installation or audio hardware. Native/managed test processes have deadlines and saved output. The native mock's concurrent volume observation now holds the same state lock as its writer. Repeated tests with unused user-volume variables were removed; real slider changes are covered by the managed graph fixture.
- Export provenance and inputs are validated before copying; source archives can supply an explicit commit. `docs/USAGE.md` is the sole package-usage source. The SDK is pinned and native release compilation uses `/Brepro`. An unused privilege-elevation function was removed.
- The first hosted CI run exposed an FMOD completion case where an inactive voice still accepted property setters. Playback helpers now check activity before updating/resuming an existing voice and share the same retirement check with the worker sweep. A native developer shell's exported `Platform=x64` no longer silently changes managed output paths; `Directory.Build.props` keeps their default build layout stable.
- Issues are enabled for bug reports. Root `LICENSE` explicitly records that no project-wide license has been granted. The FMOD notice records the missing original agreement and distribution evidence; it does not manufacture permission.

## Failure recovery requires a new session protocol

The current 250ms lease limits how long an event-thread stall can leave an external loop audible. The FMOD TAIL DSP clears **external** output when the lease expires; native channel volume is restored on failover. A contract failure prevents adapter installation. These are distinct paths, not a universal "mute native, then fallback" sequence.

Simply extending the lease to 1–2 seconds increases the allowed stale-audio interval. Simply resetting `backendReady` after queue overflow can replay stale PLAY/PAUSE/REMOVED state. Today's generation tombstones prevent removed channels from being revived; clearing them just to shrink a map defeats that protection.

A follow-up recovery design needs:

1. Separate failure reasons for temporary scheduler delay, lost critical state, native restore failure and managed contract/reflection failure.
2. A session epoch and an explicit quiesce/reset acknowledgement: stop and clear every old external voice before native/external ownership changes.
3. Sample acknowledgements and channel snapshots tied to the new epoch; stale queued events must be rejected. Already-native playback cannot be silently promoted midway through a lifetime.
4. Fault-injection tests for delayed producer/consumer threads, handle reuse, lost stop/pause and recovery during an active loop, followed by hardware listening tests.

Lease tuning can then be measured against the audible-stale-loop budget. Queue events must be classified before any lossy coalescing; dropping STOP/PAUSE or a gain/mute change is not equivalent to dropping diagnostics.

## Remaining performance and architecture work

Sample publication still hashes by handle with linear open addressing, but each sample now retains its ACK slot index. The hit/start path reads one slot and validates its full handle, requested generation and ready generation. Collisions, deleting another colliding sample and slot reuse cannot bypass these checks. Publication/backend loading may still scan 4096 entries; changing that capacity/insertion policy is separate from removing the scan on every hit.

Ended channel payloads are removed from the backend scan table after recording their retired generation. Fresh/paused lifetimes remain tracked. Round2 now bounds retired tombstones with an eviction watermark, described below, while preserving late-event rejection.

Managed field/property metadata and typed weak-reference readers are cached with weak type keys. Graph traversal reuses per-thread path/source buffers, handles nested re-entry separately, clears strong source references after each call and still snapshots current bindings under their existing lock. No gain value or mutable graph edge is cached across calls. A synthetic two-level graph measured 832 bytes/call before versus 200 bytes/call after warmup on the same runtime; this is not a real-game allocation or latency measurement.

The monitor query lock scope and retired tombstone bound are addressed below. Polling frequency remains a profiling follow-up: a larger interval can delay short-sample completion.

The public path uses an EXE launcher, a PowerShell watcher, a backend and an injected native/managed adapter. PowerShell policy bypass and injection can trigger security-product policy. No measured false-positive rate is available. A native watcher and signed distribution can reduce packaging friction, but injection remains an architectural dependency. Do not instruct users to disable antivirus protections.

The opt-in `StartupHook.Initialize` route is an internal legacy/test entry, not a supported early-attach mode. Public scripts use live attach and never set global `DOTNET_STARTUP_HOOKS`. Removing the legacy route and splitting production `.cpp` units should be separate changes with existing hook coverage retained.

FMOD 2.02.24's original agreement and upstream project-wide licensing need authentic owner/vendor evidence before a broad license can be granted. A new LICENSE filename or copying current web terms cannot establish that evidence.

## Round2 A2 shared diagnostic configuration

The backend parses settings by key and publishes startupDebug before backendReady=1. The hook validates and opens the session mapping before reading this immutable startup choice or starting AsyncTextLog. Log paths use wide characters. DllMain starts the initialization thread and performs no config/log file I/O; readValue, initDebugLogSetting and the ANSI config/log path cache were removed.

A1 reserves the complete round2 layout so the protocol magic changes once, from 0x41534932 to **0x41534933**. The fields after head/tail are startupDebug, diagnosticsEnabled, failureHead/failureTail and a 256-entry channel/generation failure ring, followed by the existing sample ACK and event arrays. The native backend and hook must be rebuilt and delivered together. The managed bridge uses its existing separate attachment ABI and does not read this native sharepool layout. The event Local\OsuAsioV2Wake_<pid> is a separate kernel object.

## Round2 A3 monitor and hook lock scope

Mixer_ChannelFlags without a PAUSE mask, ChannelFlags without a LOOP mask and ChannelSetAttribute outside VOL/PAN/FREQ pass directly to their originals before taking stateLock. VOL remains serialized because native suppression is part of route ownership.

The monitor copies channel/generation/play state and a playback revision under stateLock, queries BASS_ChannelIsActive outside it, then revalidates handle, generation and playback revision before writing state or publishing an event. Free/reuse or same-generation pause/resume during the query discards that observation. A named locked publisher builds the event from the revalidated ChannelInfo.

Native volume restoration and the remaining mutating hook calls keep their existing lock scope. They serialize ownership and volume writes against allocation/restart; moving these setters outside the lock needs a per-channel operation token or a handle-lifetime fence, then a revalidation/repair pass. A future change should profile lock duration and add controlled release/reuse and BASS re-entry tests before narrowing these sections. Windows CRITICAL_SECTION supports recursive entry; the locked publisher makes the ownership requirement explicit.

## Round2 B1 local loop failure and future epoch recovery

Loop voices receive FMOD priority 0; one-shot voices keep priority 128. Actual stolen/invalid loop handles and unexpectedly inactive loops become Finished and publish a channel/generation failure in a single-producer/single-consumer ring. The native monitor restores only the matching externally owned lifetime. Other voices keep playing, and a failed lifetime never silently re-enters external output. A full failure ring clears backendReady because losing this notification loses ownership state. Unexpected non-handle FMOD failures, queue overflow and lease loss retain global fail-closed behavior.

Round4 implements Running -> Requested -> BackendStopped -> Replaying -> Running. The lease DSP, hook or backend can atomically latch Requested on a 250 ms heartbeat expiry. Refreshing a heartbeat cannot clear it. Once attached, the monitor has a separate heartbeat so a producer stall also closes the lease. backendReady=0 remains permanent; no recovery code sets it back to 1.

The backend stops every old FMOD channel and releases its sample resources before advancing epoch and acknowledging BackendStopped. Until that acknowledgement the hook defers native restoration. Under the existing sample-publication/state/queue lock order, the hook restores native ownership, drops old queued data, invalidates sample ACKs and records a generation cutoff. It republishes the retained PCM mappings and a FIFO recovery commit marker. The backend reloads and ACKs each sample before handling the commit and reopening the lease. Failed native restore, failed sample reload, event overflow and failure-ring overflow remain permanent fail-closed paths.

Every queued event has an epoch; old epochs are rejected. The generation cutoff additionally rejects a delayed old producer even if it queues after the transition. Old active lifetimes remain native; a new allocation, stop-created lifetime or explicit restart can acquire ASIO after recovery. The shared protocol magic is now 0x41534935, distinct from the discarded experimental implementation; the pair must ship together.

RecoveryIntegration links the actual hook detours (mock BASS originals) and actual backend worker in separate translation units over one shared pool, with real FMOD NOSOUND and the production lease DSP. It suspends the consumer across the lease twice, validates stop before native restoration, actual sample reload ACKs, old-lifetime rejection, handle reuse and explicit restart, producer heartbeat expiry, failed restoration and permanent failure. OutputRegression also covers missing hook acknowledgement, old-epoch and current-epoch/old-generation replay, failed reload and a commit marker that cannot undo permanent failure. These are device-free checks; hardware listening validation is still required.

## Round2 B2 bounded retired history

GenerationHistory keeps at most 4096 retired FIFO records plus live lifetime entries. When a retired record is evicted, maxEvictedGeneration retains its replay protection. Events without an exact active channel/generation match at or below this watermark are rejected; the exact active lifetime remains valid even if its generation is older than the watermark. New generation PLAY can build its complete snapshot after eviction.

Property-only orphan events cannot accumulate live history entries or advance a live lifetime to a future generation. REMOVED records a tombstone even if CREATE was missing. Retirement of an already-evicted old generation is ignored. The queue stores one current retirement per enqueue; stale FIFO entries from handle reuse can be discarded safely because a newer identity is still retained. Serial comparisons assume fewer than 2^31 generation increments separate a delayed event and the current session state, as in the existing generation comparisons. This is a per-session bound, not a cross-session replay protocol.

## Round2 C1 contract maintenance proposal (design only)

Keep all 80 Tier1 structure rules, 41 Tier2 method projections and the embedded reviewed catalog unchanged in this round. Future slicing for AudioManager..ctor, BassAudioMixer.createMixer and DrawableRuleset construction/load should select bridge-relevant member calls and field reads/writes, include their constant/data dependencies and retain the controlling branches and exception edges. Dropping a guard or treating calls as an unordered set would weaken the check. Whole normalized method/type identity stays available to Tier3. A schema/policy revision and negative mutations are required before adopting slices.

Resolve compiler-generated members by declaring context, signature, captured-field shape and unique call-site relationships instead of ordinal names such as b__33_0 or DisplayClass3_0. Zero or ambiguous matches must fail; renaming or renumbering alone can be accepted only after the semantic slice still matches. Negative cases must retain detection of changed gain arithmetic, reversed pause flags, missing mixer creation and GameplayMixer propagation.

A scheduled CI design can fetch a pinned ppy/osu release artifact, record its URL/hash/version and inspect osu.Game plus osu.Framework without executing the game. It would run Tier1/2 and upload a candidate diff/report, notifying maintainers of drift. It must not append reviewedContracts, replace the runtime catalog or publish a build automatically. Real-client review and listening tests remain necessary before changing policy.

The optional one-shot tail proposal is also design only. A future event flag must distinguish natural decoder completion from explicit stop/pause/remove; only a non-looping natural-completion orphan could finish externally. Explicit ChannelStop, mixer pause, chart exit and every loop must stop promptly. That distinction needs dedicated event-order and shutdown tests before implementation.

## Follow-up: lease sampling and failed recovery restart

Two boundary defects were reproduced after the round4 candidate. A reader captures `now` before loading a concurrently written heartbeat; a heartbeat one tick ahead used to underflow unsigned age and latch a false recovery. All lease readers now use a common bounded comparison: age below 250 ms is fresh, as is a concurrent lead below 250 ms, including tick wrap. Age or lead at 250 ms still rejects. Expiry remains latched until the handshake completes. AudioLeaseMock reproduces the original four failures and checks both exact boundaries.

An explicit restart after recovery used to publish a new generation/epoch before BASS confirmed success. If BASS rejected the restart, a later ordinary PLAY could incorrectly acquire ASIO. The hook now saves the native lifetime, prepares suppression without committing a generation, and restores it on failure; a confirmed restart commits the new generation. It re-finds the entry after the original call and validates generation/revision before changing bookkeeping, so re-entry cannot leave it using a stale iterator. A regression verifies failed restart, subsequent ordinary PLAY staying native, and a later successful explicit restart.

## Round4 PLAY query lock revision

The PLAY hook snapshots generation and playbackRevision under stateLock, runs BASS_ChannelIsActive outside the lock, then revalidates both identities before acting. A changed identity takes a serialized retry so a stale inactive result cannot restart a resumed or reused voice. Explicit restart needs no active query. Regression faults mutate state from another thread during the real hook's query: unchanged, same-generation resume, generation replacement and release.

Tracked PAN/FREQ writes remain serialized with their bookkeeping. A generation check alone does not prevent two writes of the same attribute completing in one order and being recorded in the opposite order. Volume writes/restoration and other ownership-changing operations also retain their lock; further narrowing requires ordering and lifetime fencing, not just stale-generation rejection.

## Round4 logging revision

Managed attachment logs and the hook file logger now start new UTF-8 files with one BOM. Append/reopen preserves existing bytes, including older BOM-less logs. The backend emits a preamble only on redirected stdout; interactive consoles and stderr are untouched. This lets Windows PowerShell 5.1 detect UTF-8 while an empty stderr still means no errors. The watcher consumes the preamble using its existing StreamReader.

DiagnosticLogProbe checks the real native file append/reopen paths and redirected status bytes. ManagedLogProbe invokes the production managed writer and checks exact bytes for new, existing BOM and legacy BOM-less files. A real Start-Process redirection fixture also verifies that the watcher recognizes ASIO_READY and leaves stderr empty.

## Round3 findings from a v1.1.2 user session

One Debug Log=1 session (5.5 minutes, 1625 external PLAY events) produced a 1.6 MB backend log and a 1.4 MB hook log. Queue delay was p50/p95/max 0.0056/0.0143/0.2113 ms with missingSample=0. The logs showed three costs unrelated to audio correctness:

- After every unpause the backend issued three FMOD queries (`GetPaused`, `IsPlaying`, `GetAudibility`) whose only consumer was a debug line. They now run only when both Debug Log=1 and `playback-trace.enabled` are present.
- `BASS_StreamFree` on streams that never passed through `SampleGetChannel` (music, previews, mixer sources) published REMOVED with generation 0. The backend always rejects generation 0, so 2015 of 4790 rejected events in that session were pure queue traffic. The hook now skips untracked handles; tracked lifetimes still publish exactly one REMOVED.
- Expected late-event rejections, per-voice/channel lines in the backend, and the per-allocation SampleGetChannel, already-published and start/pause/context lines in the hook were most of both logs. They are now opt-in detailed traces that need Debug Log=1 plus `playback-trace.enabled`. Sample publication, failures and the five-second status remain with Debug Log=1 alone; the status adds `finished` and `staleRejected` counters, so rejection volume stays visible without one line per event. Replaying that session through the new filters leaves 220 of 17172 backend lines (15.8 KB of 1584 KB) and 507 of 14438 hook lines (41 KB of 1353 KB).

New regressions cover a freed untracked stream (no event) versus a tracked one (exactly one REMOVED), hook per-hit lines appearing only with the trace marker while sample publication stays in Debug Log output, and backend status counters for one natural finish plus four rejected late events with tracing off. The round3 build passed `Build.ps1` and `Test-Core.ps1`; NativeHookMock now has 303 checks in 26 cases and OutputRegression 352 checks in 13 stages, with zero failures.

A read-only replay of the same session found 1590 of 1625 voices finished naturally and 35 were paused by the game. Twelve pauses occurred within one 15.6 ms tick of the sample's natural end and none earlier, so this session gives no evidence that explicit stops truncate one-shot tails. The tail-deferral proposal remains design only.

The session's client reported Game 2026.1115 and Framework 2026.1114 and still matched a reviewed exact contract. The exact contract hashes the audio implementation types, not the assembly version, so an assembly version bump without audio changes stays `[Verified Contract]`. Documentation now names reviewed implementation families instead of implying only two client builds are accepted.
