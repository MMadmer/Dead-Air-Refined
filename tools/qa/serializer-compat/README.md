# Serializer compatibility QA

`Run-SerializerCompatibilityQa.ps1` runs the exact x64 `xrEngine.exe` from the
game root with an isolated appdata directory below `ResultRoot`. It rejects an
x86 executable, a non-empty result directory, and any result path inside the
user appdata tree. Unless explicitly skipped, it also requires the deployed
serializer DLLs and compatibility Lua script to match the current Release
build/package bytes.

The runtime matrix covers the ten legacy `before_save` `.scoc` cases (existing
and absent inputs for no-op, append, delete, create, and zero-byte output), the
incremental-prepare/synchronous-encode `capture_encode == false` abort contract,
and an in-session malformed `.scov` rejection that must leave the active world
running. Each run verifies log proof, exact companion bytes, complete
`.scop`/`.scov` output where applicable, transaction residue, and unchanged user
saves.

```powershell
pwsh -NoProfile -File .\tools\qa\Run-SerializerCompatibilityQa.ps1 `
    -FixtureRoot <isolated-fixture-savedgames-directory> `
    -ResultRoot <new-or-empty-result-directory>
```

The two pure Python models require no game process and write optional evidence
files when an output path is supplied:

```powershell
python .\tools\qa\serializer-compat\transaction_fault_model.py <results.json>
python .\tools\qa\serializer-compat\crash_state_model.py <crash-state-table.md>
python .\tools\qa\serializer-compat\verify_production_overlap.py
```

The transaction model covers helper/serial fallback, missing companions,
zero-byte custom data, copy fallback policy, prepared signatures, immediate I/O
failures, and crash recovery. The checkpoint model separately validates the
frozen companions-first/main-last recovery decisions at ten durable states.
The production verifier binds those modeled invariants to the current worker,
lease, flush, fallback, signature, and commit ordering in
`alife_storage_manager.cpp`.
