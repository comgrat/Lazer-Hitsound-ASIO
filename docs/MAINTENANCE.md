# Review decisions and maintenance plan

The community review of v1.1.0 identified real portability, testing and maintenance costs. This record distinguishes implemented fixes from changes that need a protocol design or external authorization.

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

Epoch recovery is a design only in this round. The intended state machine is Running(epoch) -> Quiescing(epoch) -> NativeOnly -> Rebuilding(epoch+1) -> Running(epoch+1). A missed lease must first silence and stop all old external voices, retire their generations and require an explicit hook acknowledgement that native restoration/quiescence completed. Heartbeats alone cannot authorize recovery. Sample ACKs and a producer snapshot must be bound to the next epoch; old queued events remain rejected. Only newly allocated/restarted generations may acquire an external route after recovery; already-native active lifetimes remain native. Queue overflow means state loss and requires a fresh session, rather than the temporary-stall recovery path.

Before implementing that state machine, fault injection must pause producers and consumers independently, cross the lease boundary with a loop active, delay/reorder old-epoch notifications, force handle reuse and prove old audio is stopped before new ownership. It must also cover missing quiesce acknowledgements, failed native restoration and sample reload. This round tests local loop loss and keeps the 250 ms DSP lease; it does not claim to validate automatic epoch recovery.

## Round2 B2 bounded retired history

GenerationHistory keeps at most 4096 retired FIFO records plus live lifetime entries. When a retired record is evicted, maxEvictedGeneration retains its replay protection. Events without an exact active channel/generation match at or below this watermark are rejected; the exact active lifetime remains valid even if its generation is older than the watermark. New generation PLAY can build its complete snapshot after eviction.

Property-only orphan events cannot accumulate live history entries or advance a live lifetime to a future generation. REMOVED records a tombstone even if CREATE was missing. Retirement of an already-evicted old generation is ignored. The queue stores one current retirement per enqueue; stale FIFO entries from handle reuse can be discarded safely because a newer identity is still retained. Serial comparisons assume fewer than 2^31 generation increments separate a delayed event and the current session state, as in the existing generation comparisons. This is a per-session bound, not a cross-session replay protocol.

## Round2 C1 contract maintenance proposal (design only)

Keep all 80 Tier1 structure rules, 41 Tier2 method projections and the embedded reviewed catalog unchanged in this round. Future slicing for AudioManager..ctor, BassAudioMixer.createMixer and DrawableRuleset construction/load should select bridge-relevant member calls and field reads/writes, include their constant/data dependencies and retain the controlling branches and exception edges. Dropping a guard or treating calls as an unordered set would weaken the check. Whole normalized method/type identity stays available to Tier3. A schema/policy revision and negative mutations are required before adopting slices.

Resolve compiler-generated members by declaring context, signature, captured-field shape and unique call-site relationships instead of ordinal names such as b__33_0 or DisplayClass3_0. Zero or ambiguous matches must fail; renaming or renumbering alone can be accepted only after the semantic slice still matches. Negative cases must retain detection of changed gain arithmetic, reversed pause flags, missing mixer creation and GameplayMixer propagation.

A scheduled CI design can fetch a pinned ppy/osu release artifact, record its URL/hash/version and inspect osu.Game plus osu.Framework without executing the game. It would run Tier1/2 and upload a candidate diff/report, notifying maintainers of drift. It must not append reviewedContracts, replace the runtime catalog or publish a build automatically. Real-client review and listening tests remain necessary before changing policy.

The optional one-shot tail proposal is also design only. A future event flag must distinguish natural decoder completion from explicit stop/pause/remove; only a non-looping natural-completion orphan could finish externally. Explicit ChannelStop, mixer pause, chart exit and every loop must stop promptly. That distinction needs dedicated event-order and shutdown tests before implementation.
