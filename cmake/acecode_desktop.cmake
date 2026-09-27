# cmake/acecode_desktop.cmake — desktop shell 子目标。
#
# 通过 -DACECODE_BUILD_DESKTOP=ON 开启。默认 OFF 保持现有 CI(TUI + daemon)
# 不变。
#
# 依赖外部库 webview/webview(MIT,C++ header-style,封装 WebView2/WKWebView/
# WebKitGTK)。用 FetchContent 拉源码,不进 vcpkg.json — vcpkg 索引相对上游
# 滞后,FetchContent 直接钉 GIT_TAG 更可控。
#
# Windows 平台上 webview 的 CMake 会自己处理 WebView2 SDK 的 NuGet 解析。
# 终端用户运行时仍需 WebView2 Runtime(Win11 默认装,Win10 1803+ 可装 Edge
# Evergreen Bootstrapper)。

if(NOT ACECODE_BUILD_DESKTOP)
    return()
endif()

include(FetchContent)

if(ACECODE_DEEPIN)
    # UOS/Deepin 20 ships WebKitGTK 4.0 and Qt 5.11. Keep the dedicated
    # distribution's ABI explicit, even on build hosts with newer WebKit.
    set(WEBVIEW_WEBKITGTK_API "4.0" CACHE STRING "WebKitGTK API" FORCE)
endif()

# WebMessage additional objects expose dropped DOM File paths to the native
# host starting with newer WebView2 SDKs. Keep this explicit and reproducible:
# webview/webview 0.12.0 otherwise defaults to the much older 1.0.1150.38 SDK.
if(WIN32)
    set(_acecode_webview2_sdk_version "1.0.4078.44")
    set(WEBVIEW_MSWEBVIEW2_VERSION "${_acecode_webview2_sdk_version}" CACHE STRING
        "Microsoft WebView2 SDK version used by ACECode Desktop" FORCE)
    # FindMSWebView2 caches only the include directory. In an existing build
    # tree that can leave the previous NuGet package selected even after the
    # pinned SDK version changes, so invalidate it once per version transition.
    # Preserve an explicitly disabled built-in SDK path.
    if(NOT DEFINED WEBVIEW_USE_BUILTIN_MSWEBVIEW2 OR
       WEBVIEW_USE_BUILTIN_MSWEBVIEW2)
        if(NOT "${ACECODE_WEBVIEW2_SDK_CACHE_VERSION}" STREQUAL
               "${_acecode_webview2_sdk_version}")
            unset(MSWebView2_INCLUDE_DIR CACHE)
        endif()
        set(ACECODE_WEBVIEW2_SDK_CACHE_VERSION
            "${_acecode_webview2_sdk_version}" CACHE INTERNAL
            "WebView2 SDK version used to populate the cached include path" FORCE)
    else()
        unset(ACECODE_WEBVIEW2_SDK_CACHE_VERSION CACHE)
    endif()
    unset(_acecode_webview2_sdk_version)
endif()

# webview/webview 0.12.0 是 2024 年的 release tag。如果将来需要更新版本,
# 钉到 commit hash 优先(release tag 通常对应一个 commit)。
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG        0.12.0
    GIT_SHALLOW    TRUE
)

# 关掉 webview 自带的样例 / 文档 / 测试,只编核心库。
set(WEBVIEW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WEBVIEW_USE_STATIC_BUILD ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(webview)

# desktop 目标自身只保留入口、splash 和 WebView host。其它 desktop/native
# helper 由根 CMakeLists.txt 里的 focused support target 提供,避免链接
# acecode_testable 把 agent/TUI/web assets 全部拖入桌面壳。
if(APPLE)
    set(ACECODE_AGENT_BROWSER_HOST_SOURCE ${ACECODE_AGENT_BROWSER_HOST_MAC_SOURCE})
endif()

set(ACECODE_DESKTOP_SOURCES
    ${ACECODE_AGENT_BROWSER_HOST_SOURCE}
    ${ACECODE_DESKTOP_MAIN_SOURCE}
    ${ACECODE_DESKTOP_SPLASH_SOURCE}
    ${ACECODE_DESKTOP_WEB_HOST_SOURCE}
)
if(UNIX AND NOT APPLE)
    list(APPEND ACECODE_DESKTOP_SOURCES
        ${ACECODE_DESKTOP_LINUX_SOURCE})
endif()

# 当前选中的 shell 清单也校验，防止后续新增显式路径漏进共享登记表。
acecode_require_sources("desktop shell sources" ${ACECODE_DESKTOP_SOURCES})

# Windows 上,acecode-desktop 用 WIN32 子系统(无 console 黑窗)。
# 同时挂上顶层 CMakeLists.txt 生成的 acecode.rc(已在 ACECODE_WINDOWS_RESOURCES 里),
# 让 acecode-desktop.exe 在资源管理器/任务栏使用与 acecode.exe 相同的图标。
if(WIN32)
    add_executable(acecode-desktop WIN32
        ${ACECODE_DESKTOP_SOURCES}
        ${ACECODE_WINDOWS_RESOURCES}
    )
elseif(APPLE)
    add_executable(acecode-desktop MACOSX_BUNDLE ${ACECODE_DESKTOP_SOURCES})
else()
    add_executable(acecode-desktop ${ACECODE_DESKTOP_SOURCES})
endif()

target_include_directories(acecode-desktop PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# webview::core_static 提供 WebView2 wrapper(Windows 路径)。
target_link_libraries(acecode-desktop PRIVATE
    acecode_desktop_support
    acecode_native_bridge_support
    webview::core
    cpr::cpr
    nlohmann_json::nlohmann_json
)

if(ACECODE_DEEPIN)
    # DTK package configs can set directory-wide definitions/includes. Keep
    # their discovery in a child directory so they cannot affect the daemon,
    # tests or the other desktop support targets.
    add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/deepin
        ${CMAKE_BINARY_DIR}/deepin)
    target_compile_definitions(acecode-desktop PRIVATE ACECODE_DEEPIN=1)
    target_link_libraries(acecode-desktop PRIVATE acecode_deepin_window_effects)
endif()

if(NOT APPLE)
    # Flat development/package layouts expect the daemon binary beside the
    # desktop shell. Make `cmake --build --target acecode-desktop` produce both
    # executables on Windows and Linux.
    add_dependencies(acecode-desktop acecode)
endif()

if(WIN32)
    target_link_libraries(acecode-desktop PRIVATE
        ole32 shell32 user32 gdi32
    )
endif()

if(UNIX AND NOT APPLE)
    set(ACECODE_LINUX_ICON "${CMAKE_SOURCE_DIR}/web/public/acecode-logo.png")
    # Refresh the adjacent window/tray artwork even when only the icon changes.
    set_property(TARGET acecode-desktop APPEND PROPERTY LINK_DEPENDS
        "${ACECODE_LINUX_ICON}")
    add_custom_command(TARGET acecode-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ACECODE_LINUX_ICON}"
            "$<TARGET_FILE_DIR:acecode-desktop>/acecode-logo.png"
        COMMENT "Copying the ACECode application icon beside the Linux desktop executable"
        VERBATIM
    )
endif()

if(APPLE)
    target_link_libraries(acecode-desktop PRIVATE
        "-framework AppKit"
        "-framework ApplicationServices"
        "-framework CoreGraphics"
        "-framework QuartzCore"
        "-framework WebKit"
    )
endif()

if(APPLE)
    set(ACECODE_MACOS_ICON "${CMAKE_SOURCE_DIR}/assets/macos/acecode.icns")
    set(ACECODE_MACOS_CHANNEL_RESOURCES
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/bridge.mjs"
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/protocol.mjs"
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/package.json"
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/package-lock.json"
    )
    set_source_files_properties(${ACECODE_MACOS_CHANNEL_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/channels/whatsapp")
    set(ACECODE_MACOS_MODELS_DEV_RESOURCES
        "${CMAKE_SOURCE_DIR}/assets/models_dev/api.json"
        "${CMAKE_SOURCE_DIR}/assets/models_dev/MANIFEST.json"
        "${CMAKE_SOURCE_DIR}/assets/models_dev/LICENSE"
    )
    file(GLOB_RECURSE ACECODE_MACOS_SEED_RESOURCES
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${CMAKE_SOURCE_DIR}/assets/seed/*"
    )
    if(NOT ACECODE_MACOS_SEED_RESOURCES)
        message(FATAL_ERROR "Missing required macOS default seed resources")
    endif()
    foreach(ACECODE_MODELS_DEV_RESOURCE IN LISTS ACECODE_MACOS_MODELS_DEV_RESOURCES)
        if(NOT EXISTS "${ACECODE_MODELS_DEV_RESOURCE}")
            message(FATAL_ERROR
                "Missing required macOS models.dev resource: ${ACECODE_MODELS_DEV_RESOURCE}")
        endif()
    endforeach()
    target_sources(acecode-desktop PRIVATE
        "${ACECODE_MACOS_ICON}"
        ${ACECODE_MACOS_MODELS_DEV_RESOURCES}
        ${ACECODE_MACOS_CHANNEL_RESOURCES}
        ${ACECODE_MACOS_SEED_RESOURCES}
    )
    set_source_files_properties("${ACECODE_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_source_files_properties(${ACECODE_MACOS_MODELS_DEV_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/share/acecode/models_dev"
    )
    foreach(ACECODE_SEED_RESOURCE IN LISTS ACECODE_MACOS_SEED_RESOURCES)
        file(RELATIVE_PATH ACECODE_SEED_RELATIVE_PATH
            "${CMAKE_SOURCE_DIR}/assets/seed"
            "${ACECODE_SEED_RESOURCE}"
        )
        get_filename_component(ACECODE_SEED_RELATIVE_DIR
            "${ACECODE_SEED_RELATIVE_PATH}" DIRECTORY)
        if(ACECODE_SEED_RELATIVE_DIR STREQUAL "")
            set(ACECODE_SEED_BUNDLE_LOCATION
                "Resources/share/acecode/seed")
        else()
            set(ACECODE_SEED_BUNDLE_LOCATION
                "Resources/share/acecode/seed/${ACECODE_SEED_RELATIVE_DIR}")
        endif()
        set_source_files_properties("${ACECODE_SEED_RESOURCE}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "${ACECODE_SEED_BUNDLE_LOCATION}")
    endforeach()
    set_target_properties(acecode-desktop PROPERTIES
        # Keep the app bundle user-facing while avoiding a case-insensitive
        # collision with the bundled daemon binary copied below.
        RUNTIME_OUTPUT_NAME "ACECode"
        MACOSX_BUNDLE_BUNDLE_NAME "ACECode"
        MACOSX_BUNDLE_ICON_FILE "acecode.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.acecode.desktop"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/macos/ACECodeDesktopInfo.plist.in"
        MACOSX_BUNDLE_INFO_STRING "ACECode Desktop"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ACECODE_BUILD_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "ACECode contributors"
    )
    add_dependencies(acecode-desktop acecode acecode-computer-use)
    set_property(TARGET acecode-desktop APPEND PROPERTY
        LINK_DEPENDS $<TARGET_FILE:acecode> $<TARGET_FILE:acecode-computer-use>)
    add_custom_command(TARGET acecode-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
            $<TARGET_FILE_DIR:acecode-desktop>/acecode-desktop
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:acecode>
            $<TARGET_FILE_DIR:acecode-desktop>/acecode-daemon
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:acecode-computer-use>
            $<TARGET_FILE_DIR:acecode-desktop>/acecode-computer-use
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_DIR:acecode-desktop>/../acecode-desktop.app"
        COMMENT "Copying acecode daemon into ACECode.app bundle"
        VERBATIM
    )

endif()

if(MSVC)
    # MultiThreaded 与 vcpkg static triplet 运行时一致(MT/MTd)。
    set_target_properties(acecode-desktop PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
