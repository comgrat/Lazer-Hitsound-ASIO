# Audio contract policy (v1.1.2)

The contract is checked before any Harmony patch is installed. It is a compatibility check over a bounded set of audio dependencies, not a proof of account safety, correctness of an arbitrary new client, or audible output.

## Modes

`Contract Mode = 1` selects **Adaptive** (the default, also used when the field is absent in a legacy five-field INI). `Contract Mode = 0` selects **Strict**. Other values and duplicate fields are rejected. Restart the game after changing mode. A per-PID attachment-status mapping freezes the backend's startup choice; edits to the INI cannot change an attachment in progress.

Both modes require Tier 1 and Tier 2. Strict additionally requires Tier 3. A hit at Tier 3 displays `[Verified Contract]`; a Tier 1/2 success with exact drift displays `[Compat/Adaptive]` and an explicit compatibility notice. Failure lists concrete members/methods with `[Contract Tier1]`, `[Contract Tier2]`, or `[Contract Tier3]` in `logs/managed-{PID}.log`. Native output remains available; an in-game Hitsound setting of 0 can make native fallback silent.

## Three tiers

1. **Structure and signatures:** reviewed target types, base types, patched method signatures, constructor sets, accessed fields/properties, generated-method uniqueness and native-facing enum values. Field types, static/instance shape, visibility, implemented interfaces/type attributes, enum underlying types and relevant accessor signatures are checked.
2. **Critical semantic projection:** metadata operands are resolved to member identities; branch/switch targets are normalized; constants, arithmetic, call/field order, locals and exception-handling ranges are retained. Only NOPs and short/long branch encoding differences are ignored in the critical methods. A set of token names is insufficient: adding a dead call, exchanging play/pause operations or replacing multiplication with addition must fail.
3. **Exact reviewed pair:** the v1.0 normalized per-type implementation hashes are retained, with multiple reviewed Game/Framework combinations. Version and MVID are recorded as evidence, not used alone as compatibility identities. Both assembly snapshots must match the same reviewed entry and its policy.

Adaptive is deliberately conservative. It tolerates changes to methods outside the critical anchor set and NOP drift in an anchor. Changes to a critical method's logging, control flow, locals or compiler-generated layout can still require review. It does not automatically support every small client update. This bounded projection is not a general semantic-equivalence solver.

## Schema

`managed/audio-contract.json` uses `schemaVersion: 2`:

```json
{
  "schemaVersion": 2,
  "profiles": [{
    "id": "audio-v1",
    "structure": [{"id": "member-id", "assembly": "osu.Framework", "type": "...", "kind": "field", "name": "channel", "signatures": ["..."]}],
    "anchors": [{"id": "method-id", "assembly": "osu.Framework", "type": "...", "method": "OnStateChanged", "signature": "instance OnStateChanged(...):...", "instructions": ["..."]}]
  }],
  "reviewedContracts": [{
    "id": "review-id", "policy": "audio-v1",
    "framework": {"assembly": "osu.Framework", "version": "...", "mvid": "...", "types": {"...": "sha256"}},
    "game": {"assembly": "osu.Game", "version": "...", "mvid": "...", "types": {"...": "sha256"}}
  }]
}
```

The shipped profile has 80 structural rules and 41 critical method projections. Reviewed combinations include Game1026/Framework1019 and Game1110/Framework1030. Runtime validation reads the **embedded** catalog; editing a loose JSON beside a released DLL does not whitelist a client. Rebuild and redistribute the managed bridge after reviewing a catalog update.

## Extract a candidate

Use a .NET 10 SDK. The tool loads audio assemblies in a collectible context, reads metadata/IL, and never calls the game entry point or starts an audio device. The client directory is read-only. An installation root with exactly one valid `current` or `app-*` directory is resolved automatically; ambiguous installations require an explicit active directory.

```powershell
.\tools\contract\Update-AudioContract.ps1 -ClientDirectory C:\path\to\client -OutputFile .\artifacts\candidate.json
```

This creates an **unreviewed candidate** and `candidate.json.diff.txt`. It does not modify the runtime catalog. Exit 3 means extraction completed but structural/semantic validation failed; read the diff. Partial extraction errors remain in the candidate/report. Use a fresh output name; existing evidence is not overwritten.

After reviewing the report and relevant upstream changes and testing the client, explicitly append its exact pair:

```powershell
.\tools\contract\Update-AudioContract.ps1 -ClientDirectory C:\path\to\client -AppendReviewed -ReviewId game-build-review
```

`-CatalogFile` selects a different source catalog for testing. The append operation requires Tier 1/2 to pass, backs up the catalog, and appends only the exact pair. It never learns a new structural/semantic policy merely to make a breaking client pass. If Tier 1/2 fails, review and change the policy in source with corresponding negative tests. An explicit review flag records the maintainer's decision; the tool cannot perform a human review or listening test itself.

## Tests

The default build runs `AudioContractProbe --tiered-self-test`. It exercises the production evaluator using synthetic assemblies, exact/drift/broken cases, legacy INI parsing and the live-session ABI without a client installation.

For metadata-only integration against a compatible client:

```powershell
dotnet run --project .\tests\AudioContractProbe -c Release -- --tiered-client-test C:\path\to\client .\artifacts\contract-mutations
```

Mutations are written only to the supplied test-output directory; original DLL hashes are verified. Cases include harmless anchor NOPs, an unrelated helper, a changed BASS handle type, a changed target signature, a broken zero-volume getter, reversed mixer pause direction, aggregate multiplication replaced with addition, missing mixer creation, altered GameplayMixer name/identifier propagation, interface removal, and BASS enum value/underlying-type changes. Mutated audio methods are never invoked.
