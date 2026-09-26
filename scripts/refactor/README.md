# Refactor verification tools

These tools use Python 3.10+ and Git. They run on Windows, Linux and macOS.
All source and document inventories come from `git ls-files`; gitlinks,
untracked files and nested worktrees are excluded. No tool searches the checkout
recursively. CMake's explicitly selected generated File API reply directory is
the only generated-file inventory. Outputs use UTF-8 without a BOM.

All lint commands default to **report mode** (exit 0 with findings). Add
`--strict` for blocking validation. Operational failures, such as unreadable
input or missing File API replies, fail in either mode. Commands which perform
an explicit equivalence check (`normalize_includes --check`, line coverage and
snapshot comparison) return a nonzero exit code when the check fails.

## Source of truth

`src/layers.tsv` is a UTF-8 TSV with columns:

`kind, path, module, group, rank, rule, target, owner, expires, note`

- `module` rows define canonical prefixes, globally unique module names,
  groups and ranks. CMake consumers should select only these rows.
- `forbid`, `restrict`, `thirdparty`, `symbol` and `allow` define semantic
  restrictions and exact single-outlet/contact-table registrations.
- `exception` requires a source glob, target glob, rule, owner, reason and an
  ISO date. It expires on that date. Strict mode requires no exception rows.
- `exempt` records the agreed web and stb scope exclusions for R12/R15.
- `ownership_allow` records an exact canonical file, named C++ scope, metric,
  owner, reason and maximum count (`rank` column). It cannot exempt another
  file, another scope, or excess occurrences. This is for approved RAII
  primitives and private-constructor/third-party factories, not whole folders.
- R4 is reserved because the approved design does not define it.

`src_layout_map.tsv` uses `old_path, new_path, phase, kind, note`.
Longest matching path wins; directory keys and replacements end in `/`.
`move` maps entire files, `delete` records separately authorized P0 removals,
and `extract` records planned splits and **never** participates in a blind
replacement. Some exact rows describe planned variants which are absent on a
particular base; `validate_map` lists these explicitly.

Layer checks use the canonical mapping during P0-P2. `--layout final` rejects
legacy source roots. The default `auto` switches to final as soon as grouped
source files exist; use `--layout transition` explicitly during a rehearsal.
The mapping does not authorize deleting files. Mutation is limited to explicitly
selected normalization, baseline capture and migration modes described below.

## Commands

```sh
python scripts/layers/check_layers.py --output layers.json
python scripts/layers/check_layers.py --enforce-parent-includes --output layers.json
python scripts/layers/check_layers.py --enforce R8 --output layers.json
python scripts/layers/check_layers.py --strict --layout final
python scripts/layers/check_file_size.py --strict
python scripts/layers/check_ownership.py --strict
python scripts/layers/check_ownership.py --final
python scripts/refactor/check_doc_paths.py --strict
python scripts/refactor/validate_map.py --strict
python scripts/refactor/normalize_includes.py --check --scope src
python scripts/refactor/normalize_includes.py --scope src
python scripts/refactor/normalize_includes.py --check --scope tests
bash scripts/refactor/branch_inventory.sh --base master --output inventory.md
```

All repository-oriented commands accept `--repo`. The branch tool can be called
directly with Python on Windows. It includes local and remote-tracking refs by
default, never fetches, and records unknown dirty status as unknown rather than
clean. `--local-only` omits remote-tracking refs. `--format json` preserves the
full `git cherry` results and worktree status records. Source/test counts are
distinct paths touched by `+` commits, so patch-equivalent historical branches
are not mistaken for unique work.

Include normalization preserves every non-path byte, mixed CRLF/LF, final
newline state, and inactive platform preprocessor branches. It refuses an
ambiguous rewrite. Same-directory bare includes are left alone as required by
P1; R8 separately reports ambiguity with generated headers. Test helpers get
their full `test_support/` prefix after the P1 file moves.

R15 is a lexical inventory, not a C++ lifetime proof. It masks comments, quoted
strings and raw strings, distinguishes deleted functions from delete
expressions, and excludes borrowed thread parameters. Synchronous CV predicates,
standard-library algorithms, immediately invoked lambdas, and local closures
used only as direct calls are reported separately. Unsafe capture entries
are tagged `escaping-context` or `requires-review`; uncertain lifetimes remain
counted rather than silently waived. `--final` additionally enforces the
phase-one ownership targets. Review actual occurrences when approving a
baseline. The optional raw-handle category implements the sixth row in the
ownership design (the mother task lists five categories).

P1 should use `--enforce-parent-includes`. Full R8 also checks root-level bare
headers and generated-name ambiguity; those are resolved in P2 before the final
strict gate.

`--write-baseline` explicitly captures the R12 or R15 baseline. Do not run it as
part of ordinary CI validation. Reduce reviewed limits when code shrinks.

## CMake, gtest and line coverage

```sh
python scripts/refactor/cmake_target_snapshot.py --build-dir build-verify --query
cmake -S . -B build-verify <normal project configuration flags>
python scripts/refactor/cmake_target_snapshot.py --build-dir build-verify --configuration Release --output targets.json
python scripts/refactor/cmake_target_snapshot.py --build-dir build-verify --configuration Release --map scripts/refactor/src_layout_map.tsv --reverse-map --compare old-targets.json
python scripts/refactor/gtest_inventory.py --binary build-verify/tests/acecode_unit_tests --run --ctest-dir build-verify --output gtest.json
python scripts/refactor/check_line_coverage.py --source-ref pre-split --map lines.tsv --original original-file.cpp
```

Snapshots enumerate **all** File API targets, including `EXCLUDE_FROM_ALL`, and
compare every `(configuration, target, source, language, definitions, options)`
tuple. Source-level definitions are included. Absolute source/build roots are
normalized only at path boundaries; a similarly named adjacent directory is
not rewritten. The build must be configured after creating the File API query.

gtest inventories retain parameterized and disabled case names, actual XML
SKIPs with reasons, failures, executed cases and the ctest registration list.
`--xml` and `--list-file` can consume previously captured output. Without a run
or XML input, `actual_results` is `null`, not an empty SKIP list. Baseline test
failures are recorded; a missing XML result or timeout is a capture error.

Line mappings have columns `old_path, old_start, old_end, new_path, new_start,
new_end, mode, reason` with one-based inclusive ranges. Every original line must
appear exactly once; every destination must be tracked and in bounds. Default
`copy` mappings verify actual line bytes (apart from newline style). `edited`
ranges require an explicit reason. Deleted/unmapped ranges cannot pass.
Repeat `--original` to detect complete files accidentally omitted from a map.

## Verification

```sh
python -m unittest discover -s scripts/refactor/tests -p 'test_*.py' -v
```

The tests create temporary Git repositories, preserve mixed line endings,
exercise nested-worktree isolation and deliberate rule violations, and configure
a real CMake C++ project with an excluded executable and source-specific defines.
A working CMake C++ compiler is required for that integration test.

## Legacy branch migration (P2-09)

`migrate_branch.py` has five modes. `rebase` and `patch` always create a **new
isolated repository**, outside all existing worktrees. They do not fetch, push,
change source refs, modify a user's checkout/index, or initialize submodules.
The destination must not already exist. It remains available on failure, with
conflicts and the full JSON report under `.git/refactor-migration/`.

```sh
python scripts/refactor/migrate_branch.py rebase origin/legacy --onto POST_FREEZE_SHA --destination ../migration-rebase --output rebase.json
python scripts/refactor/migrate_branch.py patch origin/legacy --onto POST_FREEZE_SHA --destination ../migration-patch --output patch.json
python scripts/refactor/migrate_branch.py --apply-map --repo ../migration-patch --dry-run
python scripts/refactor/migrate_branch.py --apply-map --repo ../migration-patch
python scripts/refactor/migrate_branch.py --docs --repo ../migration-patch
python scripts/refactor/migrate_branch.py --docs --repo ../migration-patch --seed-version YYYY-MM-DD.N
python scripts/refactor/migrate_branch.py --check --repo ../migration-patch --output check.json
```

- `rebase` uses `--rebase-merges --no-update-refs` in the isolated repository,
  preserving the legacy commit sequence and letting Git follow directory moves.
  The source and target SHA and merge base are pinned before cloning. `--base`
  selects another verified ancestor when the branch requires it.
- `patch` migrates the aggregate merge-base-to-branch delta, including files
  outside C++. It projects both old and new blobs **before** producing a binary,
  full-index patch, then actually runs `git apply --3way --index`. It never edits
  hunk text or leaves stale blob hashes. A companion `patch-objects.bundle`
  supplies the transformed base objects required to apply the exported patch in
  another repository (`git fetch BUNDLE migration-patch-base migration-patch-head`
  before `git apply --3way --index PATCH`). Git's raw diagnostics and unmerged
  paths are retained. Patch-equivalent commits and merge history are visible in
  the `cherry` record; this aggregate route does not claim to preserve commits.
- `--apply-map` moves indexed files and normalizes includes/build references
  using the longest path mapping. Includes are resolved against the old file
  inventory, so a bare include follows a file moved to a different module.
  Root CMake variables and module-relative paths inside each CMakeLists.txt are
  resolved in their actual source directory; included CMake files do not invent
  a CMAKE_CURRENT_SOURCE_DIR context. Explicit relative document links are
  likewise resolved from the document's directory, never from a substring.
  Mixed EOLs, missing final newlines, binary data and all unrelated bytes stay
  unchanged. The plan refuses collisions, untracked destinations, symlinks,
  ambiguous includes, deletes and semantic extraction rows before writing.
  There is no automatic delete or semantic split. A changed legacy file which
  was deleted upstream blocks patch generation; extraction issues remain
  explicit even if Git happens to apply cleanly. `--dry-run` writes nothing.
- `--docs` updates authored root/docs paths, then runs the help builder in a
  temporary snapshot containing only indexed inputs. Only tracked generated
  outputs are copied back; `sources.json` and `search-index.js` are never regex
  edited. A failed builder prevents **all** document writes. Active OpenSpec
  designs receive a map fingerprint note; their historical text, archive and
  specs are preserved. `--docs --seed-version VERSION` is a **separate seed-only
  transaction/commit**: mapped SKILLs, seed.version, MANIFEST bundle_version,
  canonical-LF hashes and exact prior bundle-version test literals change
  together. Older upgrade fixtures are retained. The current packaged test reads
  the bundle version dynamically, so there are no current literals to replace.
- `--check` is read-only and **blocking**, unlike the report-mode lint tools.
  It requires zero final-layout R1-R14 findings, no R15 ratchet increases,
  normalized includes, valid maps/document paths/seed hashes, no pending build
  path rewrites, no conflict markers and no unmerged index entries. It does not
  silently update a baseline. Four-platform build/target/test acceptance and
  semantic review are still separate requirements.

Before P2/P3 finishes, use `--layout current` to rehearse the real unchanged
layout. Final migration refuses an unfinished target by default. `--projection`
explicitly creates a **synthetic directory/include fixture**, retains unfinished
deletions and records missing semantic extractions. It is not a buildable final
baseline and never means P2/P3 has completed. This option also permits inspecting
an `--apply-map` projection with reported issues; it still exits nonzero when
issues exist. There is no projection bypass for `--check`.

The isolated clones use a read-only shared object store and disable pushes to
the source. Keep the source repository available while reviewing them. To retain
a migrated branch independently, run `git repack -a` inside that isolated clone
and remove its alternates file after verifying the repack, or transfer the branch
with a normal Git bundle. The tool does not clean up any user directory.

The fixed P0-02 nine-ref inventory has a reproducible runner:

```sh
python scripts/refactor/tests/rehearse_legacy_refs.py --base FIXED_SHA --output-dir REPORT_DIRECTORY --jobs 3
```

It executes current-layout rebase/patch and explicitly synthetic projection
patches in separate temporary repositories and verifies original refs, index and
working diff stay unchanged. `--scenario projection-patch` selects only that
scenario. Git conflicts are recorded outcomes, not a reason to discard a report
or silently merge a feature branch. All reports retain full diagnostics.
