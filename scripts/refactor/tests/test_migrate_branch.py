"""Real Git integration fixtures for all five migration modes."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from layout import LayoutMap
from migrate_branch import apply_map, check, docs, map_input, migrate
from migration_git import git, isolated_clone, revision
from migration_paths import canonical_hash, rewrite_paths


class MigrationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="acecode-migration-test-")
        self.addCleanup(self.temp.cleanup)
        self.parent = Path(self.temp.name)
        self.root = self.parent / "source"
        self.root.mkdir()
        git(self.root, "init", "-b", "master")
        git(self.root, "config", "user.name", "Migration tests")
        git(self.root, "config", "user.email", "tests@example.invalid")
        git(self.root, "config", "core.autocrlf", "false")
        self.map = self.root / "layout.tsv"
        self.rows = [("src/old/", "src/base/utils/", "move"), ("src/other/", "src/adapters/tool/", "move"), ("src/old/special.hpp", "src/domain/session/special.hpp", "move"), ("tests/old/", "tests/utils/", "move")]
        self.write_map()

    def write(self, path, data):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
        return file

    def write_map(self):
        data = "old_path\tnew_path\tphase\tkind\tnote\n" + "".join(f"{old}\t{new}\tP2\t{kind}\tfixture\n" for old, new, kind in self.rows)
        self.map.write_bytes(data.encode())

    def commit(self, message):
        git(self.root, "add", "--all")
        git(self.root, "commit", "-m", message)
        return revision(self.root, "HEAD")

    def branch_fixture(self, *, final=True):
        self.original = b'#include "../other/header.hpp"\r\n' + b"".join(f"line {i}\n".encode() for i in range(24)) + b"last-without-newline"
        self.write("src/old/file.cpp", self.original)
        self.write("src/other/header.hpp", b"#pragma once\n")
        self.write("src/old/图像.bin", b"\0\xffold\r\n")
        self.base = self.commit("base")
        git(self.root, "switch", "-c", "legacy")
        self.write("src/old/file.cpp", self.original.replace(b"line 4\n", b"feature 4\n"))
        self.write("src/old/图像.bin", b"\0\xffnew\r\n")
        self.source = self.commit("legacy feature")
        git(self.root, "switch", "master")
        if final:
            result = apply_map(self.root, self.map)
            self.assertTrue(result["success"], result)
            self.commit("move modules")
        path = "src/base/utils/file.cpp" if final else "src/old/file.cpp"
        self.write(path, (self.root / path).read_bytes().replace(b"line 20\n", b"target 20\n"))
        self.onto = self.commit("independent target change")
        return path

    def test_mapped_patch_uses_real_three_way_blobs_and_preserves_binary_unicode_eol(self):
        path = self.branch_fixture()
        refs = git(self.root, "show-ref")
        index = (self.root / ".git/index").read_bytes()
        self.write("untracked.txt", b"keep me\xff")
        dirty = (self.root / path).read_bytes() + b"\nprivate dirty change"
        self.write(path, dirty)
        report = migrate(self.root, "patch", "legacy", "master", self.parent / "result", self.map)
        self.assertTrue(report["success"], report)
        self.assertEqual(0, report["git_apply"]["returncode"])
        expected = self.original.replace(b"../other/header.hpp", b"tool/header.hpp").replace(b"line 4\n", b"feature 4\n").replace(b"line 20\n", b"target 20\n")
        self.assertEqual(expected, (Path(report["destination"]) / path).read_bytes())
        self.assertEqual(b"\0\xffnew\r\n", (Path(report["destination"]) / "src/base/utils/图像.bin").read_bytes())
        self.assertEqual(refs, git(self.root, "show-ref"))
        self.assertEqual(index, (self.root / ".git/index").read_bytes())
        self.assertEqual(dirty, (self.root / path).read_bytes())
        self.assertEqual(b"keep me\xff", (self.root / "untracked.txt").read_bytes())
        self.assertFalse(report["integration_ready"])
        # A patch consumer must receive its transformed base blobs too.
        other = isolated_clone(self.root, self.parent / "consumer")
        git(other, "switch", "--detach", self.onto)
        git(other, "fetch", report["patch_objects_bundle"], "migration-patch-base", "migration-patch-head")
        git(other, "apply", "--3way", "--index", "--whitespace=nowarn", report["patch"])
        self.assertEqual(expected, (other / path).read_bytes())

    def test_current_layout_rehearsal_does_not_normalize_or_claim_final_layout(self):
        path = self.branch_fixture(final=False)
        report = migrate(self.root, "patch", "legacy", "master", self.parent / "current", self.map, layout="current")
        self.assertTrue(report["success"], report)
        result = (Path(report["destination"]) / path).read_bytes()
        self.assertIn(b'"../other/header.hpp"\r\n', result)
        self.assertEqual("current", report["layout"])
        self.assertFalse(report["projection"])

    def test_unfinished_baseline_requires_explicit_projection_and_retains_deletions(self):
        self.branch_fixture(final=False)
        with self.assertRaisesRegex(ValueError, "not the final grouped layout"):
            migrate(self.root, "patch", "legacy", "master", self.parent / "refused", self.map)
        self.assertFalse((self.parent / "refused").exists())
        self.write("src/retired.cpp", b"must not be silently deleted\n")
        self.commit("unfinished deletion")
        self.rows.append(("src/retired.cpp", "-", "delete"))
        self.write_map()
        report = migrate(self.root, "patch", "legacy", "master", self.parent / "projection", self.map, projection=True)
        self.assertTrue(report["projection"])
        self.assertIn("NOT completed", git(Path(report["destination"]), "show", "-s", "--format=%s", report["target_sha"]).decode())
        self.assertTrue((Path(report["destination"]) / "src/retired.cpp").is_file())
        self.assertTrue(any(i["kind"] == "deleted_source" for i in report["projection_target_issues"]))

    def test_rebase_keeps_legacy_commits_and_follows_directory_rename(self):
        self.branch_fixture()
        before = git(self.root, "show-ref")
        report = migrate(self.root, "rebase", "legacy", "master", self.parent / "rebased", self.map)
        self.assertTrue(report["success"], report)
        result = Path(report["destination"])
        self.assertIn(b"feature 4", (result / "src/base/utils/file.cpp").read_bytes())
        self.assertEqual(before, git(self.root, "show-ref"))
        self.assertIn("legacy feature", git(result, "log", "-1", "--format=%s").decode())

    def test_rebase_and_patch_preserve_conflict_state_in_the_isolated_repository(self):
        self.branch_fixture(final=False)
        self.write("src/old/file.cpp", (self.root / "src/old/file.cpp").read_bytes().replace(b"line 4\n", b"different 4\n"))
        self.commit("overlapping target change")
        for mode in ("patch", "rebase"):
            report = migrate(self.root, mode, "legacy", "master", self.parent / mode, self.map, layout="current")
            self.assertFalse(report["success"])
            self.assertEqual(["src/old/file.cpp"], report["conflicts"])
            self.assertTrue(Path(report["report_file"]).is_file())
            self.assertIn("different 4", (self.root / "src/old/file.cpp").read_text())

    def test_changed_deleted_file_is_not_dropped_or_resurrected(self):
        self.branch_fixture()
        self.rows.append(("src/old/file.cpp", "-", "delete"))
        self.write_map()
        report = migrate(self.root, "patch", "legacy", "master", self.parent / "deleted", self.map)
        self.assertFalse(report["success"])
        self.assertIsNone(report["git_apply"])
        self.assertEqual("", report["status"])
        self.assertIn("deleted upstream", report["reason"])

    def test_semantic_extract_is_reported_even_when_git_apply_succeeds(self):
        self.branch_fixture()
        self.rows.append(("src/old/file.cpp", "src/domain/session/extract.cpp", "extract"))
        self.write_map()
        report = migrate(self.root, "patch", "legacy", "master", self.parent / "extract", self.map)
        self.assertEqual(0, report["git_apply"]["returncode"])
        self.assertFalse(report["success"])
        self.assertTrue(any(i["kind"] == "semantic_extract" for i in report["issues"]))

    def test_apply_map_is_idempotent_and_updates_cross_module_bare_include(self):
        self.write("src/old/file.cpp", b'#if WINDOWS\r\n#include "special.hpp"\n#endif\r\n// untouched \xff')
        self.write("src/old/special.hpp", b"#pragma once\r\n")
        self.write("CMakeLists.txt", b'set(SRC "src/old/file.cpp")\r\n# web/src/old/file.cpp\n')
        self.write("tests/cpp_source_paths.json", b'{"file": "src/old/file.cpp"}')
        self.commit("fixture")
        self.write("src/old/untracked.cpp", b"ignored")
        first = apply_map(self.root, self.map)
        self.assertTrue(first["success"], first)
        self.assertEqual(b'#if WINDOWS\r\n#include "session/special.hpp"\n#endif\r\n// untouched \xff', (self.root / "src/base/utils/file.cpp").read_bytes())
        self.assertIn(b"web/src/old/file.cpp", (self.root / "CMakeLists.txt").read_bytes())
        self.assertEqual(b"ignored", (self.root / "src/old/untracked.cpp").read_bytes())
        self.assertEqual({}, apply_map(self.root, self.map)["moves"])
        self.assertEqual([], apply_map(self.root, self.map)["content_updates"])

    def test_apply_map_refuses_untracked_collision_and_ambiguous_include_before_any_write(self):
        self.write("src/old/file.cpp", b"old\r\n")
        self.commit("fixture")
        self.write("src/base/utils/file.cpp", b"user content")
        with self.assertRaisesRegex(ValueError, "already exists"):
            apply_map(self.root, self.map)
        self.assertEqual(b"old\r\n", (self.root / "src/old/file.cpp").read_bytes())
        self.assertEqual(b"user content", (self.root / "src/base/utils/file.cpp").read_bytes())
        self.write("src/old/a.hpp", b"#pragma once\n")
        self.write("src/a.hpp", b"#pragma once\n")
        self.write("src/old/ambiguous.cpp", b'#include "a.hpp"\n')
        git(self.root, "add", "--", "src/old/a.hpp", "src/a.hpp", "src/old/ambiguous.cpp")
        # Exercise ambiguity without the unrelated pre-existing destination.
        other = self.parent / "ambiguous"
        other.mkdir()
        git(other, "init", "-b", "master")
        for p in ("src/old/a.hpp", "src/a.hpp", "src/old/ambiguous.cpp"):
            file = other / p
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes((self.root / p).read_bytes())
        git(other, "add", ".")
        report = apply_map(other, self.map)
        self.assertFalse(report["applied"])
        self.assertEqual("include", report["issues"][0]["kind"])
        self.assertTrue((other / "src/old/ambiguous.cpp").is_file())

    def test_apply_map_does_not_enter_a_real_nested_worktree(self):
        self.write("src/old/file.cpp", b"original\n")
        self.commit("fixture")
        nested = self.root / ".worktrees/fixture"
        git(self.root, "worktree", "add", "--detach", str(nested), "HEAD")
        original_index = (nested / ".git").read_bytes()
        apply_map(self.root, self.map)
        self.assertEqual(b"original\n", (nested / "src/old/file.cpp").read_bytes())
        self.assertEqual(original_index, (nested / ".git").read_bytes())
        with self.assertRaisesRegex(ValueError, "inside a user worktree|outside the source"):
            isolated_clone(self.root, nested / "should-not-exist")

    def test_map_rejects_collisions_traversal_and_existing_destination_repository(self):
        self.write("src/old/a.cpp", b"a\n")
        self.commit("fixture")
        for old, new in (("src/old/", "../escape/"), ("src/old/", "C:/escape/")):
            self.rows = [(old, new, "move")]
            self.write_map()
            with self.assertRaises(ValueError):
                apply_map(self.root, self.map)
        self.rows = [("src/old/a.cpp", "src/base/utils/A.cpp", "move"), ("src/other.cpp", "src/base/utils/a.cpp", "move")]
        self.write_map()
        with self.assertRaisesRegex(ValueError, "collision"):
            map_input(self.map, ["src/old/a.cpp", "src/other.cpp"])
        with self.assertRaisesRegex(ValueError, "must not exist"):
            isolated_clone(self.root, self.parent)

    def test_byte_path_replacement_has_both_boundaries_and_is_nonrecursive(self):
        mapping = LayoutMap([{"old_path": old, "new_path": new, "kind": kind} for old, new, kind in self.rows])
        before = b'src/old/file.cpp\r\nweb/src/old/file.cpp src/old_sibling/a src/old/special.hpp.old\n"src/old" src/old/special.hpp:5 ${CMAKE_SOURCE_DIR}/src/old/file.cpp ./src/old/file.cpp ${WEB_ROOT}/src/old/file.cpp'
        expected = b'src/base/utils/file.cpp\r\nweb/src/old/file.cpp src/old_sibling/a src/base/utils/special.hpp.old\n"src/base/utils" src/domain/session/special.hpp:5 ${CMAKE_SOURCE_DIR}/src/base/utils/file.cpp ./src/base/utils/file.cpp ${WEB_ROOT}/src/old/file.cpp'
        self.assertEqual(expected, rewrite_paths(before, mapping))

    def docs_fixture(self, failing=False):
        self.write("src/base/utils/file.cpp", b"// migrated\n")
        self.write("README.md", b"`src/old/file.cpp`\r\n`web/src/old/file.cpp`\nend")
        self.write("docs/help-source/group1.py", b'PATH = "src/old/file.cpp"\n')
        code = b'from pathlib import Path\nfrom group1 import PATH\nassert Path(PATH).is_file(), PATH\n'
        code += b'raise RuntimeError("fixture builder failure")\n' if failing else b'Path("docs/help-source/sources.json").write_text(PATH)\nPath("docs/help/page.html").write_text("built:" + PATH)\nPath("docs/help/assets/search-index.js").write_text("index:" + PATH)\n'
        self.write("docs/help-source/build_help.py", code)
        for path in ("docs/help-source/sources.json", "docs/help/page.html", "docs/help/assets/search-index.js"):
            self.write(path, b"generated src/old/file.cpp")
        self.write("openspec/changes/active/design.md", b"history src/old/file.cpp\r\n")
        self.write("openspec/changes/archive/old/design.md", b"archived src/old/file.cpp\r\n")
        self.write("openspec/specs/x/spec.md", b"spec src/old/file.cpp\r\n")
        self.commit("docs fixture")

    def test_docs_use_authored_source_and_builder_preserve_history_and_bytes(self):
        self.docs_fixture()
        self.write("docs/untracked.md", b"src/old/file.cpp")
        result = docs(self.root, self.map)
        self.assertTrue(result["success"])
        self.assertEqual(b"`src/base/utils/file.cpp`\r\n`web/src/old/file.cpp`\nend", (self.root / "README.md").read_bytes())
        self.assertEqual("built:src/base/utils/file.cpp", (self.root / "docs/help/page.html").read_text())
        self.assertEqual("index:src/base/utils/file.cpp", (self.root / "docs/help/assets/search-index.js").read_text())
        self.assertTrue((self.root / "openspec/changes/active/design.md").read_bytes().endswith(b"history src/old/file.cpp\r\n"))
        self.assertEqual(b"archived src/old/file.cpp\r\n", (self.root / "openspec/changes/archive/old/design.md").read_bytes())
        self.assertEqual(b"src/old/file.cpp", (self.root / "docs/untracked.md").read_bytes())
        self.assertEqual([], docs(self.root, self.map)["files"])

    def test_failed_help_build_rolls_back_entire_document_plan_and_dry_run_does_not_write(self):
        self.docs_fixture(failing=True)
        original = (self.root / "README.md").read_bytes()
        with self.assertRaisesRegex(ValueError, "before any document was written"):
            docs(self.root, self.map)
        self.assertEqual(original, (self.root / "README.md").read_bytes())
        self.write("docs/help-source/build_help.py", b"print('generated fixture')\n")
        report = docs(self.root, self.map, dry_run=True)
        self.assertIn("README.md", report["files"])
        self.assertEqual(original, (self.root / "README.md").read_bytes())

    def seed_fixture(self):
        self.skill = "assets/seed/skills/acecode/sample/SKILL.md"
        original = b"src/old/file.cpp\r\nweb/src/old/file.cpp\nlast"
        self.write(self.skill, original)
        self.write("assets/seed/seed.version", b"2026-09-17.1\r\n")
        manifest = {"bundle_version": "2026-09-17.1", "skills": [{"relative_path": "acecode/sample", "skill_md_sha256": canonical_hash(original)}]}
        self.write("assets/seed/MANIFEST.json", json.dumps(manifest, indent=2).replace("\n", "\r\n").encode() + b"\r\n")
        self.write("tests/skills/default_skill_seeder_test.cpp", b'expected = "2026-09-17.1";\r\nold_fixture = "2026-07-20.1";\n')
        self.write("README.md", b"src/old/file.cpp\n")
        self.commit("seed fixture")
        return original

    def test_seed_only_bump_preserves_manifest_bytes_and_updates_hash_and_exact_test_literal(self):
        original = self.seed_fixture()
        before = (self.root / "assets/seed/MANIFEST.json").read_bytes()
        report = docs(self.root, self.map, version="2026-09-27.1")
        self.assertEqual("seed-only", report["scope"])
        expected = original.replace(b"src/old/file.cpp\r", b"src/base/utils/file.cpp\r")
        self.assertEqual(expected, (self.root / self.skill).read_bytes())
        after = (self.root / "assets/seed/MANIFEST.json").read_bytes()
        self.assertEqual(before.replace(b"2026-09-17.1", b"2026-09-27.1").replace(canonical_hash(original).encode(), canonical_hash(expected).encode()), after)
        self.assertEqual(b'expected = "2026-09-27.1";\r\nold_fixture = "2026-07-20.1";\n', (self.root / "tests/skills/default_skill_seeder_test.cpp").read_bytes())
        self.assertEqual(b"src/old/file.cpp\n", (self.root / "README.md").read_bytes())
        self.assertEqual([], docs(self.root, self.map, version="2026-09-27.1")["files"])

    def test_seed_requires_new_version_and_valid_original_manifest_before_writing(self):
        original = self.seed_fixture()
        with self.assertRaisesRegex(ValueError, "newer"):
            docs(self.root, self.map, version="2026-09-17.1")
        self.assertEqual(original, (self.root / self.skill).read_bytes())
        self.write(self.skill, original + b"changed without hash")
        with self.assertRaisesRegex(ValueError, "hash disagrees"):
            docs(self.root, self.map, version="2026-09-27.1")
        self.assertEqual(b"2026-09-17.1\r\n", (self.root / "assets/seed/seed.version").read_bytes())

    def test_check_enforces_final_layout_lint_ownership_seed_and_paths_without_writing(self):
        self.seed_fixture()
        docs(self.root, self.map, version="2026-09-27.1")
        self.write("README.md", b"Final source: src/base/utils/file.cpp\n")
        self.write("src/base/utils/file.cpp", b"// final fixture\n")
        for path in ("src/layers.tsv", "scripts/layers/ownership_baseline.json", "scripts/layers/size_baseline.txt", "scripts/refactor/src_layout_map.tsv"):
            self.write(path, (HERE.parent.parent / path).read_bytes())
        self.commit("complete final fixture")
        before = git(self.root, "status", "--porcelain=v1")
        report = check(self.root, self.map)
        self.assertTrue(report["success"], report["counts"])
        self.assertEqual(before, git(self.root, "status", "--porcelain=v1"))
        self.write("src/base/utils/file.cpp", b'<<<<<<< branch\nstd::thread worker;\n#include "../missing.hpp"\n')
        failed = check(self.root, self.map)
        self.assertFalse(failed["success"])
        self.assertGreater(failed["counts"]["layers"], 0)
        self.assertGreater(failed["counts"]["ownership"], 0)
        self.assertGreater(failed["counts"]["migration_paths"], 0)


if __name__ == "__main__":
    unittest.main()
