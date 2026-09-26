from datetime import date
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SCRIPTS / "refactor"))
sys.path.insert(0, str(SCRIPTS / "layers"))
from check_layers import inspect as check_layers
from check_file_size import inspect as check_sizes
from check_ownership import apply_allowances, scan as scan_ownership
from check_doc_paths import inspect as check_docs
from check_line_coverage import inspect as check_lines
from cmake_target_snapshot import snapshot, write_query, compare, normalize_root_paths, normalize_source_path
from gtest_inventory import parse_list, parse_xml
from layout import IncludeIndex, LayerPolicy, LayoutMap, include_matches, load_policy, mask_cpp
from normalize_includes import normalize, run as normalize_repo
from repo_files import git, tracked_files
from validate_map import inspect as validate_map


class GuardTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        git(self.root, "init", "-b", "master")
        git(self.root, "config", "user.email", "tests@example.invalid")
        git(self.root, "config", "user.name", "Guard tests")
        self.policy = load_policy(SCRIPTS.parent)
        self.files = []

    def write(self, path, data):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
        if path not in self.files:
            self.files.append(path)
        return file

    def rules(self, **kwargs):
        return check_layers(self.root, self.files, self.policy, transition=False, **kwargs)

    def test_platform_includes_and_mixed_newlines_are_normalized_idempotently(self):
        # 未激活的平台条件也要规范化，二次运行必须为零改动且行尾不变。
        path = "src/tool/x.cpp"
        data = b'#if defined(_WIN32)\r\n#include "../utils/a.hpp"\n#endif\r\n#include "local.hpp"'
        self.write(path, data)
        self.write("src/tool/local.hpp", b"\n")
        self.write("src/utils/a.hpp", b"\n")
        git(self.root, "add", ".")
        first = normalize_repo(self.root, False)
        self.assertEqual(1, first["changed_lines"])
        expected = data.replace(b"../utils/a.hpp", b"utils/a.hpp")
        self.assertEqual(expected, (self.root / path).read_bytes())
        self.assertEqual(0, normalize_repo(self.root, True)["changed_lines"])

    def test_normalization_does_not_touch_nested_worktrees_or_untracked_files(self):
        # 脚本从仓库根运行时也不得进入三种嵌套 worktree 或未跟踪文件。
        self.write("src/utils/a.hpp", b"\n")
        self.write("src/tool/x.cpp", b'#include "../utils/a.hpp"\n')
        git(self.root, "add", ".")
        ignored = [".claude/worktrees/x/src/z.cpp", ".worktrees/x/src/z.cpp", ".acecode/worktrees/x/src/z.cpp", "src/untracked.cpp"]
        before = b'#include "../utils/a.hpp"\r\n'
        for path in ignored:
            self.write(path, before)
        normalize_repo(self.root, False)
        for path in ignored:
            self.assertEqual(before, (self.root / path).read_bytes())

    def test_real_nested_git_worktrees_are_untouched(self):
        # 用真正的嵌套 Git worktree 验证，避免仅凭同名普通目录推断安全。
        self.write("src/utils/a.hpp", b"\n")
        self.write("src/tool/x.cpp", b'#include "../utils/a.hpp"\r\n')
        git(self.root, "add", ".")
        git(self.root, "commit", "-m", "tracked source")
        nested = [self.root / prefix / "fixture" for prefix in (".claude/worktrees", ".worktrees", ".acecode/worktrees")]
        for path in nested:
            self.assertTrue(path.resolve().is_relative_to(self.root.resolve()))
            git(self.root, "worktree", "add", "--detach", str(path), "HEAD")
        before = [(path / "src/tool/x.cpp").read_bytes() for path in nested]
        normalize_repo(self.root, False)
        self.assertEqual(before, [(path / "src/tool/x.cpp").read_bytes() for path in nested])

    def test_normalization_rejects_ambiguous_include(self):
        # 同时命中包含者目录与模块根时，不能按搜索顺序静默选中。
        path = "src/apps/tui/x.cpp"
        files = [path, "src/apps/tui/utils/a.hpp", "src/base/utils/a.hpp"]
        data = b'#include "utils/a.hpp"\n'
        after, changes, errors = normalize(data, path, IncludeIndex(files))
        self.assertEqual(data, after)
        self.assertFalse(changes)
        self.assertEqual(2, len(errors[0]["targets"]))

    def test_p1_helper_references_follow_moves_before_normalization(self):
        # P1 先 git mv helper 再改 include；旧裸名与 ../ 均需通过映射找到新文件。
        path = "tests/agent_loop/first_test.cpp"
        files = [path, "tests/test_support/agent/stub_provider.hpp"]
        aliases = LayoutMap([{"old_path": "tests/agent_loop/stub_provider.hpp", "new_path": "tests/test_support/agent/stub_provider.hpp", "kind": "move", "phase": "P1-01"}])
        before = b'#include "stub_provider.hpp"\r\n#include "../agent_loop/stub_provider.hpp"\n'
        after, changes, errors = normalize(before, path, IncludeIndex(files, aliases=aliases))
        self.assertEqual(b'#include "test_support/agent/stub_provider.hpp"\r\n#include "test_support/agent/stub_provider.hpp"\n', after)
        self.assertEqual(2, len(changes))
        self.assertFalse(errors)

    def test_comments_and_raw_string_examples_do_not_create_fake_rules(self):
        # 注释与 raw string 中的 C++ 示例不能伪造所有权或 include 违规。
        data = b'/* new X; */\nconst char* x = R"tag(\n#include "../x.hpp"\nnew X; // not code\n)tag";\n'
        self.assertFalse(scan_ownership(data, "fixture.cpp"))
        self.assertFalse(list(include_matches(data)))
        self.assertEqual(data.count(b"\n"), mask_cpp(data).count(b"\n"))

    def test_rank_semantic_peer_and_third_party_guards(self):
        # 分组反向、同组反向、domain/provider、apps 平级与第三方越界独立生效。
        self.write("src/base/utils/x.cpp", b'#include "config/a.hpp"\n#include <ftxui/dom/node.hpp>\n')
        self.write("src/base/config/a.hpp", b"\n")
        self.write("src/domain/session/x.cpp", b'#include "provider/a.hpp"\n#include <cpr/cpr.h>\n')
        self.write("src/adapters/provider/a.hpp", b"\n")
        self.write("src/apps/tui/x.cpp", b'#include "web/a.hpp"\n')
        self.write("src/apps/web/a.hpp", b"\n")
        counts = self.rules()["counts"]
        for rule in ("R1", "R2", "R3", "R5", "R7"):
            self.assertGreater(counts[rule], 0, rule)

    def test_standard_memory_header_and_project_mcp_config_are_not_third_party(self):
        # memory 模块与标准头同名；项目 mcp_config 也不能当成 cpp-mcp。
        self.write("src/base/config/a.cpp", b'#include <memory>\n#include "mcp_config.hpp"\n')
        self.write("src/base/config/mcp_config.hpp", b"\n")
        counts = self.rules()["counts"]
        self.assertEqual(0, counts["R7"])
        self.assertEqual(0, counts["R8"])

    def test_narrow_layers_and_test_helpers(self):
        # desktop、vocab、纯 TUI model 的限制不能由一般 rank 规则替代。
        self.write("src/apps/desktop/x.cpp", b'#include "llm/a.hpp"\n')
        self.write("src/domain/llm/a.hpp", b"\n")
        self.write("src/base/config/vocab/x.hpp", b'#include <windows.h>\n')
        self.write("src/apps/tui/model/x.cpp", b'#include <ftxui/dom/node.hpp>\n')
        self.write("tests/tui/bad.hpp", b"\n")
        counts = self.rules()["counts"]
        self.assertEqual(3, counts["R6"])
        self.assertEqual(1, counts["R14"])

    def test_include_uniqueness_spelling_and_old_root_guard(self):
        # 最终树必须拒绝旧根回流、带分组前缀、尖括号项目头与歧义解析。
        self.write("src/base/utils/a.hpp", b"\n")
        self.write("src/apps/tui/utils/a.hpp", b"\n")
        self.write("src/apps/tui/x.cpp", b'#include <utils/a.hpp>\n#include "base/utils/a.hpp"\n')
        self.write("src/session/old.cpp", b"\n")
        counts = self.rules()["counts"]
        self.assertGreaterEqual(counts["R8"], 3)
        self.assertGreater(counts["R9"], 0)
        self.assertGreater(counts["R10"], 0)

    def test_bare_project_header_is_only_allowed_in_its_own_directory(self):
        # 测试里从 include 根找到的裸项目头不属于同目录例外。
        self.write("src/permissions.hpp", b"\n")
        self.write("tests/permissions/x_test.cpp", b'#include "permissions.hpp"\n')
        self.assertEqual(1, self.rules()["counts"]["R8"])

    def test_single_outlet_is_a_real_symbol_check(self):
        # 把单出口符号放进未登记文件时必须失败，注释中的同名文案可保留。
        self.write("src/engine/agent/request/rogue.cpp", b'model_facing_provider_messages();\nmessages_.push_back(x);\nprovider->chat(req);\nmessages_.back().content += text;\n')
        self.write("src/base/utils/comments.hpp", b'// record_audit(x);\n')
        self.assertEqual(4, self.rules()["counts"]["R11"])

    def test_exception_requires_owner_expiry_and_strict_empty(self):
        # 例外到期、缺字段和 strict 仍留例外都必须失败，不能长期漂白违规。
        row = {key: "" for key in self.policy.rows[0]}
        row.update(kind="exception", path="src/base/utils/*", rule="R2", target="src/base/config/*", owner="fixture", expires="2030-01-01", note="temporary forwarding header")
        self.policy.rows.append(row)
        self.write("src/base/utils/x.cpp", b'#include "config/a.hpp"\n')
        self.write("src/base/config/a.hpp", b"\n")
        report = self.rules(today=date(2026, 9, 27))
        self.assertEqual(0, report["counts"]["R2"])
        self.assertEqual(1, len(report["exceptions_used"]))
        self.assertGreater(self.rules(today=date(2030, 1, 1))["counts"]["R13"], 0)
        self.assertGreater(self.rules(today=date(2026, 9, 27), strict=True)["counts"]["R13"], 0)

    def test_size_ratchet_rejects_growth_and_tracks_web_exemption(self):
        # 基线文件增长一行仍失败；web 豁免必须明确出现在结果里。
        self.write("src/base/utils/old.cpp", b"x\n" * 1002)
        self.write("src/base/utils/new.cpp", b"x\n" * 1001)
        self.write("src/apps/web/server.cpp", b"x\n" * 2000)
        report = check_sizes(self.root, self.files, self.policy, {"src/base/utils/old.cpp": 1001})
        self.assertEqual(2, len(report["findings"]))
        self.assertEqual(1, len(report["exemptions"]))

    def test_ownership_categories_ignore_deleted_functions_and_literals(self):
        # 删除函数不是 delete 表达式；异步回调与延迟 setter 则应计入棘轮。
        code = b'''Thing(const Thing&) = delete;
void* operator new(size_t);
const char* text = "new Thing; delete p;";
auto p = new Thing;
delete p;
std::thread worker([this] { work(); });
worker.detach();
void set_registry(Registry* value);
sqlite3* database_;
'''
        metrics = [r["metric"] for r in scan_ownership(code, "x.cpp")]
        for metric in ("raw_new", "raw_delete", "detach", "std_thread", "unsafe_capture", "delayed_injection", "raw_handle"):
            self.assertEqual(1, metrics.count(metric), metric)

    def test_primitive_allowance_is_limited_to_exact_scope_and_count(self):
        # 原语自 join 的一次 detach 可登记，但同文件其它函数或新增第二次不能豁免。
        code = b'''namespace thread_detail {
inline void join_or_detach_self(std::thread& thread) {
    thread.detach();
    thread.detach();
}
void unrelated() { worker.detach(); }
}
'''
        found = scan_ownership(code, "src/utils/joining_thread.hpp")
        remaining, allowed = apply_allowances(found, self.policy)
        self.assertEqual(1, len(allowed))
        self.assertEqual(2, len(remaining))
        self.assertTrue(all(item["metric"] == "detach" for item in found))

    def test_synchronous_predicates_are_separate_from_escaping_callbacks(self):
        # cv predicate、本地直接调用与 IIFE 都在当前调用内结束；提交到队列仍要报。
        code = b'''void owner() {
auto local = [&] { work(); };
local();
cv.wait(lock, [this] { return ready_; });
[&] { work(); }();
queue.enqueue([this] { work(); });
auto escapes = [&] { work(); };
queue.enqueue(escapes);
}'''
        synchronous = []
        found = scan_ownership(code, "x.cpp", synchronous)
        self.assertEqual(3, len(synchronous))
        self.assertEqual(2, sum(item["metric"] == "unsafe_capture" for item in found))

    def test_map_rejects_collisions_and_unsafe_prefixes(self):
        # Windows 文件名大小写折叠后冲突必须拒绝，不能在搬迁时覆盖文件。
        mapping = LayoutMap([
            {"old_path": "src/tool/", "new_path": "src/adapters/tool/", "kind": "move", "phase": "P3"},
            {"old_path": "src/a.cpp", "new_path": "src/adapters/tool/A.cpp", "kind": "move", "phase": "P3"},
        ])
        report = validate_map(["src/tool/a.cpp", "src/a.cpp"], mapping)
        self.assertTrue(any("collision" in f["message"] for f in report["findings"]))
        self.assertEqual("src/tool_preamble/a.cpp", mapping.translate("src/tool_preamble/a.cpp"))

    def test_line_partition_detects_loss_duplication_and_wrong_destination(self):
        # 拆分后丢行、重叠映射或目标内容不符都不能用“已映射”蒙混通过。
        self.write("src/original.cpp", b"one\r\ntwo\nthree\n")
        git(self.root, "add", ".")
        git(self.root, "commit", "-m", "original")
        self.write("src/first.cpp", b"one\ntwo\n")
        self.write("src/last.cpp", b"three\n")
        git(self.root, "add", ".")
        rows = [{"old_path": "src/original.cpp", "old_start": "1", "old_end": "2", "new_path": "src/first.cpp", "new_start": "1", "new_end": "2"}, {"old_path": "src/original.cpp", "old_start": "3", "old_end": "3", "new_path": "src/last.cpp", "new_start": "1", "new_end": "1"}]
        self.assertFalse(check_lines(self.root, "HEAD", rows, ["src/original.cpp"])["findings"])
        self.assertTrue(check_lines(self.root, "HEAD", rows[:1], ["src/original.cpp"])["findings"])
        self.assertTrue(check_lines(self.root, "HEAD", rows + rows[:1], ["src/original.cpp"])["findings"])
        self.write("src/last.cpp", b"wrong\n")
        self.assertTrue(check_lines(self.root, "HEAD", rows, ["src/original.cpp"])["findings"])

    def test_doc_paths_expand_braces_and_ignore_web_src_substrings(self):
        # 大括号简写要逐项检查；web/src 不能误识别成仓库的 src 根。
        self.write("src/utils/a.hpp", b"\n")
        self.write("src/utils/a.cpp", b"\n")
        self.write("README.md", b'`src/utils/a.{hpp,cpp}` `src/missing.hpp` `web/src/valid.js`\n')
        report = check_docs(self.root, self.files)
        self.assertEqual(["src/missing.hpp"], [f["path"] for f in report["findings"]])

    def test_gtest_parameterized_inventory_and_actual_skip(self):
        # 参数化名字必须保留，真正 SKIP 与 disabled/notrun 不能混为一谈。
        names = parse_list("Suite.\n  Works\nTyped/0. # TypeParam = int\n  Case/0 # GetParam() = 7\n")
        self.assertEqual(["Suite.Works", "Typed/0.Case/0"], names)
        xml = self.write("results.xml", b'<testsuites><testsuite name="Suite"><testcase name="Works" status="run"/><testcase name="Skip" status="run" result="skipped"><skipped message="no device"/></testcase><testcase name="DISABLED_Test" status="notrun" result="suppressed"/></testsuite></testsuites>')
        report = parse_xml(xml)
        self.assertEqual([{"name": "Suite.Skip", "reason": "no device"}], report["skipped"])
        self.assertNotIn("Suite.DISABLED_Test", report["executed"])


class CMakeFileApiTest(unittest.TestCase):
    def test_comparison_keeps_tuple_multiplicity(self):
        # 规范化不能让重复元组变成集合后丢失计数；少一项仍必须报错。
        row = {"target": "core", "source": "@build/generated.cpp"}
        before = {"targets": [], "tuples": [row, row]}
        after = {"targets": [], "tuples": [row]}
        self.assertEqual([row], compare(before, after)["tuples"]["removed"])

    def test_relative_sources_distinguish_build_and_similar_source_paths(self):
        # build 在 source 内时 File API 返回相对路径；不得误归一同名前缀源码。
        self.assertEqual("@build/generated/a.cpp", normalize_source_path("build-a/generated/a.cpp", "C:/repo", "C:/repo/build-a"))
        self.assertEqual("build-a-extra/a.cpp", normalize_source_path("build-a-extra/a.cpp", "C:/repo", "C:/repo/build-a"))
        self.assertEqual("src/a.cpp", normalize_source_path("src/a.cpp", "/repo", "/repo/build"))
        self.assertEqual("@build/generated/a.cpp", normalize_source_path("/build/generated/a.cpp", "/repo", "/build"))

    def test_fresh_nested_build_directories_have_identical_source_snapshots(self):
        # 两个真正的全新构建目录必须等价，包括生成源与消费 OBJECT 库的目标。
        with tempfile.TemporaryDirectory(prefix="acecode-file-api-builds-") as temporary:
            source = Path(temporary)
            (source / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.20)\nproject(NestedBuilds LANGUAGES CXX)\nconfigure_file(core.cpp generated.cpp COPYONLY)\nadd_library(core OBJECT "${CMAKE_BINARY_DIR}/generated.cpp")\nadd_executable(smoke EXCLUDE_FROM_ALL smoke.cpp)\ntarget_link_libraries(smoke PRIVATE core)\n', encoding="utf-8")
            (source / "core.cpp").write_text("int core() { return 1; }\n", encoding="utf-8")
            (source / "smoke.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            reports = []
            for name in ("build-before", "build-after"):
                build = source / name
                write_query(build)
                result = subprocess.run(["cmake", "-S", str(source), "-B", str(build)], capture_output=True, text=True, timeout=120)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                reports.append(snapshot(build))
            changes = compare(*reports)
            self.assertTrue(any(row["source"] == "@build/generated.cpp" for row in reports[0]["tuples"]))
            self.assertFalse(any(value for delta in changes.values() for value in delta.values()), changes)

    def test_root_normalization_observes_both_path_boundaries(self):
        # repo 与 repo2 相邻前缀不得混淆，define 的引号和其它字节仍要保留。
        value = 'ROOT="C:/repo/assets" OTHER="C:/repo2/assets" OUT="C:/repo/build/generated" PREFIX="xC:/repo/assets"'
        expected = 'ROOT="@source/assets" OTHER="C:/repo2/assets" OUT="@build/generated" PREFIX="xC:/repo/assets"'
        self.assertEqual(expected, normalize_root_paths(value, "C:/repo", "C:/repo/build"))
        self.assertEqual("-I@source/include -I/tmp/repo2", normalize_root_paths("-I/tmp/repo/include -I/tmp/repo2", "/tmp/repo", "/tmp/repo/build"))

    def test_actual_codemodel_keeps_excluded_targets_and_source_defines(self):
        # EXCLUDE_FROM_ALL 不在默认构建里，但快照必须保留它及逐源定义。
        self.assertIsNotNone(shutil.which("cmake"), "CMake is required for this integration test")
        with tempfile.TemporaryDirectory(prefix="acecode-file-api-") as temporary:
            root = Path(temporary)
            source, build = root / "source", root / "build"
            source.mkdir()
            (source / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.15)\nproject(GuardFixture LANGUAGES CXX)\nadd_library(core STATIC core.cpp)\ntarget_compile_definitions(core PRIVATE TARGET_DEFINE)\nset_source_files_properties(core.cpp PROPERTIES COMPILE_DEFINITIONS SOURCE_DEFINE)\nadd_executable(smoke EXCLUDE_FROM_ALL smoke.cpp)\n', encoding="utf-8")
            (source / "core.cpp").write_text("int core() { return 1; }\n", encoding="utf-8")
            (source / "smoke.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            write_query(build)
            result = subprocess.run(["cmake", "-S", str(source), "-B", str(build)], capture_output=True, text=True, timeout=120)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            report = snapshot(build)
            self.assertIn("smoke", [target["name"] for target in report["targets"]])
            core = next(row for row in report["tuples"] if row["target"] == "core" and row["source"] == "core.cpp")
            self.assertEqual("CXX", core["language"])
            self.assertIn("SOURCE_DEFINE", core["defines"])
            self.assertIn("TARGET_DEFINE", core["defines"])
            changed = json.loads(json.dumps(report))
            changed["tuples"].pop()
            self.assertTrue(compare(report, changed)["tuples"]["removed"])


if __name__ == "__main__":
    unittest.main()
