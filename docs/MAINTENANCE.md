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

Ended channel payloads are removed from the backend scan table after recording their retired generation. Fresh/paused lifetimes remain tracked. Small generation tombstones remain until the backend session ends; they are not blindly expired, so late same-generation PLAY cannot revive a finished or removed voice.

Managed field/property metadata and typed weak-reference readers are cached with weak type keys. Graph traversal reuses per-thread path/source buffers, handles nested re-entry separately, clears strong source references after each call and still snapshots current bindings under their existing lock. No gain value or mutable graph edge is cached across calls. A synthetic two-level graph measured 832 bytes/call before versus 200 bytes/call after warmup on the same runtime; this is not a real-game allocation or latency measurement.

Monitor lock duration/polling frequency and tombstone memory bounds remain follow-ups requiring profiling and lifetime rules. A larger monitor interval can delay short-sample completion; pruning generation state without an epoch/watermark can replay stale events.

The public path uses an EXE launcher, a PowerShell watcher, a backend and an injected native/managed adapter. PowerShell policy bypass and injection can trigger security-product policy. No measured false-positive rate is available. A native watcher and signed distribution can reduce packaging friction, but injection remains an architectural dependency. Do not instruct users to disable antivirus protections.

The opt-in `StartupHook.Initialize` route is an internal legacy/test entry, not a supported early-attach mode. Public scripts use live attach and never set global `DOTNET_STARTUP_HOOKS`. Removing the legacy route and splitting production `.cpp` units should be separate changes with existing hook coverage retained.

FMOD 2.02.24's original agreement and upstream project-wide licensing need authentic owner/vendor evidence before a broad license can be granted. A new LICENSE filename or copying current web terms cannot establish that evidence.
