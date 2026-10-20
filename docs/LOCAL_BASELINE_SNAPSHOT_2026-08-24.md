# Local pre-proof baseline snapshot — 2026-08-24

> **Mandatory preflight:** before every device performance phase run
> `scripts/preflight-vivo-performance-test.ps1` as defined by
> `PERFORMANCE_TEST_PREFLIGHT.md`; evidence captured without this gate is
> invalid.

This file records the local Git state that must be preserved before the
Bloodborne proof campaign and any subsequent optimization branches. None of
these commits has been pushed to a remote.

## Source repositories

| Repository | Local branch | Snapshot commit |
| --- | --- | --- |
| `funnel-arm` | `work/sm8650-a750-vulkan-only-20260822` (also referenced by `snapshot/funnel-pre-proof-20260824`) | `0e9a1692abedae418591d6f31c457d8df46f3e4c` |
| Embedded FEX adapter | `stable/inside-8gen5-fex-callret-20260824` (also referenced by `snapshot/fex-adapter-pre-proof-20260824`) | `7a35591927e25b6c6c117ba1bbb32ca098bfb4b2` |
| Embedded FEX core | `snapshot/fex2608-pre-proof-20260824` | `56be0e219c8b51ef6a8dfd40a8b09fa3909cf046` |
| `externals/LibAtrac9` | `snapshot/lsx4-pre-proof-20260824` | `33b9e9d5b92af139e5a67b72c496ffd5fd31ef7f` |
| `externals/fmt` | `snapshot/lsx4-pre-proof-20260824` | `b3861aac1ee2dd0d0e862d3f90cdb36827680009` |
| `externals/openal-soft` | `snapshot/lsx4-pre-proof-20260824` | `f4d39fd45e4faa4db2595f2378cc1eaf64d75403` |

The parent LSX4 snapshot is the first commit that contains this file and the
gitlink updates above; the branch name is `snapshot/lsx4-pre-proof-20260824`.

## Large local evidence retained outside Git objects

Before the snapshot, the LSX4 worktree contained 13,094 untracked files using
22,061,488,681 bytes, while the drive had only about 17.54 GiB free. A literal
`git add -A` could not complete safely. The following regenerable or externally
sourced classes therefore remain untouched at their existing paths but are not
inserted into the Git object database:

- Android and host build directories (`.build-*` and generated build output);
- raw Perfetto/perf captures, maps, callgraphs and large logcat dumps;
- screenshots, frame sequences, contact sheets and temporal-analysis output;
- game packages, guest executables, extracted ELF/bin payloads and caches;
- APKs and other installable/generated binaries;
- the FEX core `.build-*` directories (about 201 MB at snapshot time).

All tracked files and untracked source, tests, scripts, documentation and
runtime configuration are part of the parent snapshot. Excluded evidence is
not deleted or moved. It must not be mistaken for committed, portable evidence.

## Proof-first rule

No optimization branch may be accepted from this baseline until its device
result has a retained successful performance preflight, exact process/backend/
driver/patch/cache fingerprint, matched scene and route, counter-provenance
gate, and frame-normalized A/B/A evidence. A rejected or contaminated run is
evidence about the test setup, not about LSX4 performance.
