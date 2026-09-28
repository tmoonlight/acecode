"""apply_include_roots:只换单独作为 include 根的 src,不碰源文件路径;幂等;换行保持。"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from apply_include_roots import apply, rewrite  # noqa: E402

ROOT_CMAKE = (
    "file(GLOB_RECURSE ACECODE_ALL_SOURCES CONFIGURE_DEPENDS\r\n"
    "    ${CMAKE_SOURCE_DIR}/src/*.cpp\r\n"
    ")\r\n"
    "file(GLOB_RECURSE ACECODE_HEADER_FILES CONFIGURE_DEPENDS\r\n"
    "    ${CMAKE_SOURCE_DIR}/src/*.hpp\r\n"
    "    ${CMAKE_SOURCE_DIR}/src/*.h\r\n"
    ")\r\n"
    "acecode_assert_known_roots(ALLOW_LEGACY SOURCES\r\n"
    "    ${ACECODE_ALL_SOURCES} ${ACECODE_HEADER_FILES} ${ACECODE_OBJCXX_SOURCES})\r\n"
    "target_include_directories(acecode_testable PUBLIC\r\n"
    "    ${CMAKE_SOURCE_DIR}/src\r\n"
    "    ${CMAKE_BINARY_DIR}/generated)\r\n"
    "target_sources(x PRIVATE ${CMAKE_SOURCE_DIR}/src/apps/cli/main.cpp)\r\n"
    "target_include_directories(native PUBLIC ${CMAKE_SOURCE_DIR}/src)\r\n"
    "add_library(acecode_testable OBJECT\r\n"
    "    ${ACECODE_TESTABLE_SOURCES}\r\n"
    "    ${ACECODE_HEADER_FILES}\r\n"
    ")\r\n"
    "source_group(TREE ${CMAKE_SOURCE_DIR} FILES\r\n"
    "    ${ACECODE_MAIN_SOURCE}\r\n"
    "    ${ACECODE_ALL_SOURCES}\r\n"
    "    ${ACECODE_HEADER_FILES}\r\n"
    ")\r\n"
)
TESTS_CMAKE = "target_include_directories(t PRIVATE\n    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external/ftxui/src)\n"


class ApplyIncludeRootsTest(unittest.TestCase):
    def test_replaces_only_bare_src_roots_and_defines_variable_once(self):
        # 触发场景:根 CMake 里 src 既作为 glob 前缀、源文件路径前缀,也单独作为 include 根出现。
        # 期望:只有单独出现的两处换成 ${ACECODE_INCLUDE_ROOTS};glob 与源文件路径原样;ALLOW_LEGACY 去掉;
        # 变量在 known-roots 断言之后定义一次;CRLF 保持。
        updated, report = rewrite(ROOT_CMAKE.encode("utf-8"), "CMakeLists.txt")
        text = updated.decode("utf-8")
        self.assertEqual(2, report["roots_replaced"])
        self.assertTrue(report["allow_legacy_removed"] and report["roots_defined"])
        self.assertIn("${CMAKE_SOURCE_DIR}/src/*.cpp", text)
        self.assertIn("${CMAKE_SOURCE_DIR}/src/apps/cli/main.cpp", text)
        self.assertIn("acecode_assert_known_roots(SOURCES\r\n", text)
        self.assertEqual(1, text.count("set(ACECODE_INCLUDE_ROOTS"))
        self.assertIn("    ${ACECODE_INCLUDE_ROOTS}\r\n    ${CMAKE_BINARY_DIR}/generated)", text)
        self.assertIn("target_include_directories(native PUBLIC ${ACECODE_INCLUDE_ROOTS})", text)
        self.assertNotIn("\n\n\r", text)
        self.assertTrue(text.index("ACECODE_OBJCXX_SOURCES})") < text.index("set(ACECODE_INCLUDE_ROOTS"))
        # stb 头搬到 external 后仍要出现在 acecode_testable 与 source_group 里(快照逐元组不变),但不进 known-roots 断言。
        self.assertTrue(report["thirdparty_headers_added"])
        self.assertEqual(1, text.count("file(GLOB ACECODE_THIRDPARTY_HEADERS"))
        self.assertEqual(2, text.count("    ${ACECODE_THIRDPARTY_HEADERS}\r\n"))
        self.assertNotIn("${ACECODE_HEADER_FILES} ${ACECODE_THIRDPARTY_HEADERS} ${ACECODE_OBJCXX_SOURCES}", text)

    def test_idempotent_and_tests_cmake_keeps_neighbouring_tokens(self):
        # 触发场景:tests/CMakeLists.txt 同一行里 src 根后面紧跟 ftxui 的 include 目录;脚本对整个仓库跑两次。
        # 期望:只换 src 根,ftxui 路径不动;第二次运行没有任何改动(正式搬迁可重复执行)。
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tests").mkdir()
            (root / "CMakeLists.txt").write_bytes(ROOT_CMAKE.encode("utf-8"))
            (root / "tests" / "CMakeLists.txt").write_bytes(TESTS_CMAKE.encode("utf-8"))
            first = apply(root)
            self.assertEqual([True, True], [r["changed"] for r in first])
            self.assertEqual("target_include_directories(t PRIVATE\n    ${ACECODE_INCLUDE_ROOTS} ${CMAKE_SOURCE_DIR}/external/ftxui/src)\n",
                             (root / "tests" / "CMakeLists.txt").read_text(encoding="utf-8"))
            second = apply(root)
            self.assertEqual([False, False], [r["changed"] for r in second])
            self.assertEqual([0, 0], [r["roots_replaced"] for r in second])


if __name__ == "__main__":
    unittest.main()
