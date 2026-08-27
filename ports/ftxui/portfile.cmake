# 本 overlay port 直接构建 external/ftxui 子模块。vcpkg 的 ABI hash 只算
# portfile + vcpkg.json 的内容,**不看** SOURCE_PATH 下的源码 —— 所以子模块
# gitlink 一变,vcpkg 仍认为已安装的那份是最新的,configure 时静默跳过重建,
# 症状是编译报 `xxx 不是 ftxui::App 的成员` 而头文件里明明有(装在
# vcpkg_installed 里的是旧 commit 编出来的)。
#
# 因此:**每次 external/ftxui 的 gitlink 变更,必须同步 bump vcpkg.json 的
# port-version**,否则改动不会生效。当前对应子模块 commit:
#   658c942c6eaceae88fd7ea4458ffa1f5a7775af7 (v7.0.3-19,含 synchronized output)
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

get_filename_component(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../external/ftxui" ABSOLUTE)

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

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/${PORT})

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
