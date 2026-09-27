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
The mapping is not a permission to move or delete files: these tools are checks,
except for the explicitly named include normalization and baseline capture modes.

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
