# 本 overlay port 直接构建 external/ftxui 子模块。vcpkg 的 ABI hash 只算
# portfile + vcpkg.json 的内容,**不看** SOURCE_PATH 下的源码 —— 所以子模块
# gitlink 一变,vcpkg 仍认为已安装的那份是最新的,configure 时静默跳过重建,
# 症状是编译报 `xxx 不是 ftxui::App 的成员` 而头文件里明明有(装在
# vcpkg_installed 里的是旧 commit 编出来的)。
#
# 因此:**每次 external/ftxui 的 gitlink 变更,必须同步 bump vcpkg.json 的
# port-version**,否则改动不会生效。当前对应子模块 commit:
#   62e7471b (含 synchronized output + opt-in mouse hover motion)
#   —— 输入追踪日志路径由 ACECODE_FTXUI_INPUT_TRACE_DIR 运行时指定。
#   —— 原子追加输入追踪记录,避免多个 TUI 进程互相覆盖。
#   —— 输入追踪日志使用 tui_input_trace_YYYY_MM_DD.log。
#   —— hover-motion 补丁(link-hover-tooltip 依赖无按键 Mouse::Moved 上报,
#   DEC mode 1003),提供 ftxui::App::EnableMouseHoverMotion()。
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

get_filename_component(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../external/ftxui" ABSOLUTE)

# Patch a private build copy so configuring dependencies never modifies the submodule.
file(REAL_PATH "${CURRENT_BUILDTREES_DIR}" ACECODE_FTXUI_BUILD_ROOT)
set(ACECODE_FTXUI_BUILD_SOURCE "${ACECODE_FTXUI_BUILD_ROOT}/acecode_runtime_source")
file(REAL_PATH "${ACECODE_FTXUI_BUILD_SOURCE}" ACECODE_FTXUI_RESOLVED_SOURCE)
get_filename_component(ACECODE_FTXUI_SOURCE_PARENT "${ACECODE_FTXUI_RESOLVED_SOURCE}" DIRECTORY)
if(NOT ACECODE_FTXUI_SOURCE_PARENT STREQUAL ACECODE_FTXUI_BUILD_ROOT)
    message(FATAL_ERROR "FTXUI private source must be directly inside its build tree")
endif()
file(REMOVE_RECURSE "${ACECODE_FTXUI_BUILD_SOURCE}")
file(COPY "${SOURCE_PATH}/" DESTINATION "${ACECODE_FTXUI_BUILD_SOURCE}" PATTERN ".git" EXCLUDE)
set(SOURCE_PATH "${ACECODE_FTXUI_BUILD_SOURCE}")

# Keep ACECode's runtime naming policy in this overlay, including cache rebuilds
# from the unchanged upstream gitlink.
set(ACECODE_TRACE_HEADER "${SOURCE_PATH}/src/ftxui/component/acecode_input_trace.hpp")
file(READ "${ACECODE_TRACE_HEADER}" ACECODE_TRACE_SOURCE)
string(REPLACE "tui-input-trace-" "tui_input_trace_" ACECODE_TRACE_SOURCE "${ACECODE_TRACE_SOURCE}")
string(REPLACE "%Y-%m-%d" "%Y_%m_%d" ACECODE_TRACE_SOURCE "${ACECODE_TRACE_SOURCE}")
file(WRITE "${ACECODE_TRACE_HEADER}" "${ACECODE_TRACE_SOURCE}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DFTXUI_BUILD_EXAMPLES=OFF
        -DFTXUI_ENABLE_INSTALL=ON
        -DFTXUI_BUILD_TESTS=OFF
        -DFTXUI_BUILD_DOCS=OFF
        -DACECODE_TUI_INPUT_TRACE=ON
)

vcpkg_cmake_install()

# The independent-process log fixture exercises the exact production writer.
file(INSTALL "${ACECODE_TRACE_HEADER}" DESTINATION "${CURRENT_PACKAGES_DIR}/include/ftxui/component")

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/${PORT})

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
