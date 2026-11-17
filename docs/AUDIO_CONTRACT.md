# Audio contract policy (v1.2.0)

The contract is checked before any Harmony patch is installed. It is a compatibility check over a bounded set of audio dependencies, not a proof of account safety, correctness of an arbitrary new client, or audible output.

## Modes

`Contract Mode = 1` selects **Adaptive** (the default, also used when the field is absent in a legacy five-field INI). `Contract Mode = 0` selects **Strict**. Other values and duplicate fields are rejected. Restart the game after changing mode. A per-PID attachment-status mapping freezes the backend's startup choice; edits to the INI cannot change an attachment in progress.

Both modes require Tier 1 and Tier 2. Strict additionally requires Tier 3. A hit at Tier 3 displays `[Verified Contract]`; a Tier 1/2 success with exact drift displays `[Compat/Adaptive]` and an explicit compatibility notice. Failure lists concrete members/methods with `[Contract Tier1]`, `[Contract Tier2]`, or `[Contract Tier3]` in `logs/managed-{PID}.log`. Native output remains available; an in-game Hitsound setting of 0 can make native fallback silent.

## Three tiers

1. **Structure and signatures:** reviewed target types, base types, patched method signatures, constructor sets, accessed fields/properties, generated-method uniqueness and native-facing enum values. Field types, static/instance shape, visibility, implemented interfaces/type attributes, enum underlying types and relevant accessor signatures are checked.
2. **Critical semantic projection:** metadata operands are resolved to member identities; branch/switch targets are normalized; constants, arithmetic, call/field order, locals and exception-handling ranges are retained. Only NOPs and short/long branch encoding differences are ignored in the critical methods. A set of token names is insufficient: adding a dead call, exchanging play/pause operations or replacing multiplication with addition must fail.
3. **Exact reviewed pair:** the v1.0 normalized per-type implementation hashes are retained, with multiple reviewed Game/Framework combinations. Version and MVID are recorded as evidence, not used alone as compatibility identities. Both assembly snapshots must match the same reviewed entry and its policy.

Adaptive is deliberately conservative. Projection revision 2 adds a narrow diagnostic slice: at the entry of AudioManager constructors, BassAudioMixer.createMixer and DrawableRuleset construction/load, an isolated `ldstr; call Console.WriteLine(string)` may be ignored only when no branch or exception boundary enters that pair. Literal input, empty entry stack and unused void result are required. Other logging, data dependencies, locals, branches, arithmetic and exception ranges remain checked; this is not general semantic equivalence.

Generated anchor members are selected within their declaring context by their complete normalized signature/body and captured field operations. Exactly one candidate must match. A consistent one-to-one ordinal mapping is established from these anchors and applied at exact operand positions in their callers. Renumbering alone may pass Adaptive, while changed arithmetic, swapped call targets, merged identities and ambiguous duplicate bodies reject. Tier1 uniqueness checks remain intact. Exact Tier3 implementation hashes still include all original names and instructions.

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

The optional profile field `projectionRevision` defaults to 1 (legacy strict projections); value 2 explicitly enables the bounded slice and generated-context mapping described above. Other values reject. The schema stays at version 2 and the reviewed exact entries are unchanged. The round5 local profile has 81 structural rules and 41 critical method projections; the additional enum rule pins OsuSetting.HitsoundVolume=121. The existing DrawableRuleset<T>.load anchor already preserves GetBindable -> hitsoundVolume -> GameplayMixer.Volume.BindTo. Binding/value mutations must fail Tier1/2. Reviewed combinations include Game1026/Framework1019 and Game1110/Framework1030. Runtime validation reads the **embedded** catalog; editing a loose JSON beside a released DLL does not whitelist a client. Rebuild and redistribute the managed bridge after reviewing a catalog update.

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

Round4 adds mutations for generated member/type/cache renumbering, renumbering with changed arithmetic, swapped delegate call targets, an ambiguous identical generated body, an isolated entry diagnostic, a branch entering the diagnostic and extra computation beside it. The current local client's real suite passed 98 assertions. Mutations are written only to the supplied test-output directory; original DLL hashes are verified. Cases include harmless anchor NOPs, an unrelated helper, a changed BASS handle type, a changed target signature, a broken zero-volume getter, reversed mixer pause direction, aggregate multiplication replaced with addition, missing mixer creation, altered GameplayMixer name/identifier propagation, interface removal, and BASS enum value/underlying-type changes. Mutated audio methods are never invoked.
