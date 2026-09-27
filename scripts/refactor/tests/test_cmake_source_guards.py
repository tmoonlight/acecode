from pathlib import Path
import subprocess
import tempfile
import unittest


GUARDS = Path(__file__).resolve().parents[3] / "cmake/acecode_source_guards.cmake"


class CMakeSourceGuardsTest(unittest.TestCase):
    def configure(self, body, files=()):
        temporary = tempfile.TemporaryDirectory(prefix="acecode-source-guards-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "source with spaces"
        source.mkdir()
        for name in files:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"// fixture\n")
        policy = source / "src/layers.tsv"
        policy.parent.mkdir(exist_ok=True)
        policy.write_text("module\tsrc/base/utils/\tutils\tbase\t0\n", encoding="utf-8")
        mapping = source / "scripts/refactor/src_layout_map.tsv"
        mapping.parent.mkdir(parents=True)
        mapping.write_text("src/utils/\tsrc/base/utils/\tP3\tmove\t-\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(SourceGuardFixture NONE)\n"
            f'include("{GUARDS.as_posix()}")\n' + body,
            encoding="utf-8",
        )
        return subprocess.run(
            ["cmake", "-S", str(source), "-B", str(root / "build")],
            capture_output=True, text=True, timeout=30,
        )

    def test_missing_explicit_sources_and_empty_lists_fail_at_configure(self):
        # 路径拼错或清单消失必须失败，不能等到目标漏编才暴露。
        for arguments in ('"src/missing.cpp"', '"src"', ''):
            with self.subTest(arguments=arguments):
                result = self.configure(f'acecode_require_sources("explicit fixture" {arguments})\n')
                self.assertNotEqual(0, result.returncode)
                self.assertIn("explicit fixture", result.stderr)

    def test_source_definitions_keep_existing_entries_and_generator_expressions(self):
        # define 迁移要保留 APPEND 语义、引号和生成表达式，不能覆盖原属性。
        body = '''set_property(SOURCE src/base/utils/a.cpp PROPERTY COMPILE_DEFINITIONS ORIGINAL)
acecode_set_source_define(src/base/utils/a.cpp "$<$<BOOL:ON>:ACECODE_DEEPIN=1>" "ASSET=\\\"directory with spaces\\\"")
get_source_file_property(actual src/base/utils/a.cpp COMPILE_DEFINITIONS)
set(expected "ORIGINAL;$<$<BOOL:ON>:ACECODE_DEEPIN=1>;ASSET=\\\"directory with spaces\\\"")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "source definition changed: ${actual}")
endif()
'''
        result = self.configure(body, ["src/base/utils/a.cpp"])
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_property_on_a_nonexistent_source_cannot_succeed_silently(self):
        # CMake 原生 set_property 接受不存在的源，本封装必须把它变成错误。
        result = self.configure('acecode_set_source_define(src/missing.cpp FEATURE=1)\n')
        self.assertNotEqual(0, result.returncode)
        self.assertIn("source compile definition", result.stderr)
        self.assertIn("src/missing.cpp", result.stderr)

    def test_final_layout_rejects_old_roots_and_similar_prefixes(self):
        # final 模式拒绝旧根回流，也不能把 utils_extra 误识别成 utils/。
        for name in ("src/utils/a.cpp", "src/base/utils_extra/a.cpp"):
            with self.subTest(name=name):
                result = self.configure(f'acecode_assert_known_roots(SOURCES "{name}")\n', [name])
                self.assertNotEqual(0, result.returncode)
                self.assertIn("outside known layout roots", result.stderr)
        result = self.configure('acecode_assert_known_roots(SOURCES src/base/utils/a.cpp)\n', ["src/base/utils/a.cpp"])
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_transition_accepts_only_registered_legacy_roots(self):
        # P0-P2 的映射许可不等于允许任意 src/ 子目录。
        result = self.configure('acecode_assert_known_roots(ALLOW_LEGACY SOURCES src/utils/a.cpp)\n', ["src/utils/a.cpp"])
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        result = self.configure('acecode_assert_known_roots(ALLOW_LEGACY SOURCES src/unknown/a.cpp)\n', ["src/unknown/a.cpp"])
        self.assertNotEqual(0, result.returncode)
        self.assertIn("outside known layout roots", result.stderr)


if __name__ == "__main__":
    unittest.main()
