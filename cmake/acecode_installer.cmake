# 自研 Windows 安装程序 acecode-setup(见 docs/windows-installer.md)。
# 安装/卸载流程与纯逻辑在 acecode_installer_support,所有平台都编译并进单测;
# 自绘窗口和入口只属于 Windows 上的 acecode-setup 可执行文件。
# 安装包本身由 installer/windows/make_setup.py 在打包阶段嵌进 exe 的资源区。
set(ACECODE_INSTALLER_BINARY_ONLY_SOURCES
    "${CMAKE_SOURCE_DIR}/src/apps/installer/installer_main_win.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/installer/installer_window_win.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/installer/installer_canvas_win.cpp")
acecode_require_sources("installer binary sources" ${ACECODE_INSTALLER_BINARY_ONLY_SOURCES})

acecode_collect_group_sources(_installer apps/installer)
list(REMOVE_ITEM _installer ${ACECODE_INSTALLER_BINARY_ONLY_SOURCES})
acecode_add_layer_library(acecode_installer_support apps ${_installer})
target_link_libraries(acecode_installer_support
    PUBLIC acecode_desktop_support
    PRIVATE libzip::zip)
if(WIN32)
    target_link_libraries(acecode_installer_support PRIVATE advapi32 shell32 ole32 uuid user32)
endif()
target_link_libraries(acecode_testable INTERFACE acecode_installer_support)

if(WIN32)
    configure_file(
        "${CMAKE_SOURCE_DIR}/assets/windows/acecode-setup.rc.in"
        "${CMAKE_BINARY_DIR}/generated/acecode-setup.rc"
        @ONLY)
    # 链接器默认嵌入 requestedExecutionLevel=asInvoker 的清单:文件名带 setup 也不会被
    # Windows 的「安装程序检测」自动要求管理员权限。
    add_executable(acecode-setup WIN32
        ${ACECODE_INSTALLER_BINARY_ONLY_SOURCES}
        "${CMAKE_BINARY_DIR}/generated/acecode-setup.rc")
    target_compile_definitions(acecode-setup PRIVATE NOMINMAX)
    target_link_libraries(acecode-setup PRIVATE acecode_installer_support gdiplus dwmapi)
endif()
