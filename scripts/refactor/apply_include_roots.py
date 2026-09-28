"""P3 M2 的结构性 CMake 改动(apply_layout 不做的部分):6 个分组 include 根替换 ${CMAKE_SOURCE_DIR}/src,去掉 ALLOW_LEGACY。

映射改写只换源文件路径;分组搬迁后 `src/` 本身不再是 include 根(design.md D1),要把三处 CMake 里所有
单独作为 include 目录出现的 `${CMAKE_SOURCE_DIR}/src` 换成 `${ACECODE_INCLUDE_ROOTS}`(6 个分组根 +
`external`(stb 等第三方头)+ `generated`(版本头)),并让 `acecode_assert_known_roots` 不再放行映射表里的旧路径。
stb 头搬到 external/stb 后不再落在 src 的头文件 glob 里;为了让 target 快照(File API 里的头文件元组)与搬迁前逐元组相同,
另加一个 ACECODE_THIRDPARTY_HEADERS glob 挂回 acecode_testable 与 source_group,但不进 known-roots 断言。
幂等:重复运行不再改动。

  python scripts/refactor/apply_include_roots.py [--repo .] [--dry-run]
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

BUILD_FILES = ("CMakeLists.txt", "tests/CMakeLists.txt", "cmake/acecode_desktop.cmake")
ROOT_TOKEN = re.compile(r"\$\{CMAKE_SOURCE_DIR\}/src(?=[\s)])")
GUARD_ANCHOR = "    ${ACECODE_ALL_SOURCES} ${ACECODE_HEADER_FILES} ${ACECODE_OBJCXX_SOURCES})\n"
ROOTS_BLOCK = '''
# P3 M2(refactor20260927 D1):6 个分组各自作为 include 根,模块根形式的 include
# ("utils/paths.hpp")在分组搬迁后不变;generated 放版本头,external 放 stb 等第三方头。
set(ACECODE_INCLUDE_ROOTS
    ${CMAKE_SOURCE_DIR}/src/base
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/adapters
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/src/host
    ${CMAKE_SOURCE_DIR}/src/apps
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_BINARY_DIR}/generated)
foreach(_root IN LISTS ACECODE_INCLUDE_ROOTS)
    if(NOT _root MATCHES "generated$" AND NOT IS_DIRECTORY "${_root}")
        message(FATAL_ERROR "include root does not exist: ${_root}")
    endif()
endforeach()
'''


def rewrite(data: bytes, path: str) -> tuple[bytes, dict]:
    """返回改写后的字节与统计;换行风格与其它字节原样保留。"""
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    text = data.decode("utf-8").replace("\r\n", "\n")
    text, replaced = ROOT_TOKEN.subn("${ACECODE_INCLUDE_ROOTS}", text)
    report = {"file": path, "roots_replaced": replaced, "allow_legacy_removed": False, "roots_defined": False, "thirdparty_headers_added": False}
    if path == "CMakeLists.txt":
        header_glob = "file(GLOB_RECURSE ACECODE_HEADER_FILES CONFIGURE_DEPENDS\n    ${CMAKE_SOURCE_DIR}/src/*.hpp\n    ${CMAKE_SOURCE_DIR}/src/*.h\n)\n"
        if "ACECODE_THIRDPARTY_HEADERS" not in text:
            if text.count(header_glob) != 1:
                raise ValueError("CMakeLists.txt: 找不到 ACECODE_HEADER_FILES 的 glob 块")
            text = text.replace(header_glob, header_glob + "file(GLOB ACECODE_THIRDPARTY_HEADERS CONFIGURE_DEPENDS\n    ${CMAKE_SOURCE_DIR}/external/stb/*.h)\n")
            for anchor in ("add_library(acecode_testable OBJECT\n    ${ACECODE_TESTABLE_SOURCES}\n    ${ACECODE_HEADER_FILES}\n",
                           "source_group(TREE ${CMAKE_SOURCE_DIR} FILES\n    ${ACECODE_MAIN_SOURCE}\n    ${ACECODE_ALL_SOURCES}\n    ${ACECODE_HEADER_FILES}\n"):
                if text.count(anchor) != 1:
                    raise ValueError("CMakeLists.txt: 找不到 ACECODE_HEADER_FILES 的用法锚点: " + anchor.splitlines()[0])
                text = text.replace(anchor, anchor + "    ${ACECODE_THIRDPARTY_HEADERS}\n")
            report["thirdparty_headers_added"] = True
        if text.count("acecode_assert_known_roots(ALLOW_LEGACY SOURCES\n") == 1:
            text = text.replace("acecode_assert_known_roots(ALLOW_LEGACY SOURCES\n", "acecode_assert_known_roots(SOURCES\n")
            report["allow_legacy_removed"] = True
        if "set(ACECODE_INCLUDE_ROOTS" not in text:
            if text.count(GUARD_ANCHOR) != 1:
                raise ValueError("CMakeLists.txt: 找不到 acecode_assert_known_roots 的 SOURCES 结尾行,无法定位 ACECODE_INCLUDE_ROOTS 的插入点")
            text = text.replace(GUARD_ANCHOR, GUARD_ANCHOR + ROOTS_BLOCK)
            report["roots_defined"] = True
    return text.replace("\n", nl.decode()).encode("utf-8"), report


def apply(root: Path, dry_run: bool = False) -> list[dict]:
    reports = []
    for relative in BUILD_FILES:
        file = root / relative
        if not file.exists():
            continue
        original = file.read_bytes()
        updated, report = rewrite(original, relative)
        report["changed"] = updated != original
        if report["changed"] and not dry_run:
            file.write_bytes(updated)
        reports.append(report)
    return reports


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    for report in apply(Path(args.repo).resolve(), args.dry_run):
        print(f"{report['file']}: roots_replaced={report['roots_replaced']} allow_legacy_removed={report['allow_legacy_removed']} "
              f"roots_defined={report['roots_defined']} thirdparty_headers_added={report['thirdparty_headers_added']} changed={report['changed']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
