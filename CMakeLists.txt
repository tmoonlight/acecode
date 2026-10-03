cmake_minimum_required(VERSION 3.20)

# Keep MSVC runtime mode consistent with vcpkg dependencies. If the user
# didn't specify a triplet explicitly, default Windows builds to the static
# triplet so dependencies are built with MT/MTd as well.
if(CMAKE_HOST_WIN32 AND DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Vcpkg target triplet" FORCE)
endif()

project(acecode VERSION 0.9.33 LANGUAGES C CXX)

include(${CMAKE_SOURCE_DIR}/cmake/acecode_source_guards.cmake)

option(ACECODE_TUI_INPUT_TRACE "Enable verbose TUI input event trace logging." OFF)
option(ACECODE_DEEPIN "Build the dedicated UOS/Deepin Linux variant." OFF)
if(ACECODE_DEEPIN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ACECODE_DEEPIN is supported only on Linux targets")
endif()
# CTest integration: defines BUILD_TESTING (default ON). Release packaging can
# disable it with -DBUILD_TESTING=OFF to skip the test binary and the gtest
# lookup that `tests/` performs.
include(CTest)
include(${CMAKE_SOURCE_DIR}/cmake/acecode_source_paths.cmake)

# The daemon needs the package identity, but must not acquire Qt/DTK linkage.
acecode_set_source_define("${ACECODE_UPGRADE_MANIFEST_SOURCE}"
    "$<$<BOOL:${ACECODE_DEEPIN}>:ACECODE_DEEPIN=1>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinGW uses COFF's small section table by default. Large generated or test
# translation units need the GNU assembler equivalent of MSVC's /bigobj.
if(MINGW)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:-Wa$<COMMA>-mbig-obj>"
    )
endif()

# MSVC: static runtime (MT/MTd), UTF-8, PDB in Release
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # The codebase intentionally uses exceptions for JSON parsing, filesystem
    # adapters, provider recovery, and daemon route validation. Without /EHsc,
    # MSVC skips standard stack-unwind semantics and those guarded paths abort
    # instead of reaching their catch blocks.
    add_compile_options(/EHsc /utf-8 /Zi /FS /bigobj)
    # /MP: 单 project 内多个 .cpp 并行编(默认关闭,串行)。Ninja 不需要也会
    # 忽略;只对 MSBuild / VS IDE 生成器有意义。不传数字 = 用全部核心,VS 里
    # 也可在命令行覆盖:cmake --build ... -- /p:CL_MPCount=28
    add_compile_options(/MP)
    add_link_options(
        /DEBUG
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>"
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>"
    )
endif()

if(NOT MSVC)
    add_compile_options(
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<CONFIG:Debug>>>:-ffunction-sections>"
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<CONFIG:Debug>>>:-fdata-sections>"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-ffunction-sections>"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-fdata-sections>"
    )
    if(APPLE)
        add_link_options("$<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>")
    elseif(UNIX)
        add_link_options("$<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>")
    endif()
endif()

# Generate version header from CMake project version
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

if(WIN32)
    enable_language(RC)
    set(ACECODE_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/windows/acecode.ico")
    set(ACECODE_NOTIFICATION_LOGO_PATH
        "${CMAKE_SOURCE_DIR}/assets/windows/acecode_icon.png")
    configure_file(
        ${CMAKE_SOURCE_DIR}/assets/windows/acecode.rc.in
        ${CMAKE_BINARY_DIR}/generated/acecode.rc
        @ONLY
    )
    set(ACECODE_WINDOWS_RESOURCES ${CMAKE_BINARY_DIR}/generated/acecode.rc)
endif()

# Embed web/ assets into acecode_web. Generates a single .cpp with byte
# arrays + a name → span lookup map. Re-runs on every cmake re-configure when
# files in web/dist/ change (CONFIGURE_DEPENDS in the cmake function).
#
# web/ is now a Vite + React + Tailwind project; the runtime assets are the
# `web/dist/` build artifact, NOT the JSX source. If `web/dist/` is missing
# the embed function emits an empty asset map (daemon will 404 the SPA but
# keep the API working) — devs need to run `pnpm install && pnpm build`
# inside web/ once before configuring cmake. CI/release flow runs the same点
# command before invoking cmake.
include(${CMAKE_SOURCE_DIR}/cmake/acecode_embed_assets.cmake)
acecode_compute_build_version(ACECODE_BUILD_VERSION)
set(ACECODE_WEB_DIST "${CMAKE_SOURCE_DIR}/web/dist")
set(ACECODE_WEB_EMBED_DIR "${ACECODE_WEB_DIST}")
if(NOT EXISTS "${ACECODE_WEB_DIST}")
    message(WARNING
        "[acecode] web/dist/ not found — embedding a minimal fallback page. "
        "Run `pnpm install && pnpm build` inside web/ before re-configuring for the full UI.")
    set(ACECODE_WEB_EMBED_DIR "${CMAKE_BINARY_DIR}/generated/web-dist-fallback")
    file(MAKE_DIRECTORY "${ACECODE_WEB_EMBED_DIR}")
    file(WRITE "${ACECODE_WEB_EMBED_DIR}/index.html"
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>ACECode</title></head>"
        "<body><main style=\"font-family: sans-serif; padding: 24px;\">"
        "<h1>ACECode Web UI assets were not built</h1>"
        "<p>Run <code>pnpm install && pnpm build</code> inside <code>web/</code>, then reconfigure CMake.</p>"
        "</main></body></html>")
endif()
acecode_generate_embedded_assets(
    ${ACECODE_WEB_EMBED_DIR}
    ${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp
    ${ACECODE_BUILD_VERSION}
)

find_package(cpr CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(Threads REQUIRED)

# cpr ships with libcurl; on Windows libcurl >= 8.14 is required for proper
# certificate handling.  Detect the version through the CURL::libcurl target
# that vcpkg exposes and fail early with a clear message.
if(WIN32)
    find_package(CURL CONFIG QUIET)
    if(CURL_FOUND AND DEFINED CURL_VERSION_STRING)
        if(CURL_VERSION_STRING VERSION_LESS "8.14.0")
            message(FATAL_ERROR
                "libcurl >= 8.14.0 is required on Windows (found ${CURL_VERSION_STRING}). "
                "Please update your vcpkg checkout so that the curl port is >= 8.14.0.")
        endif()
    endif()
endif()

# cpp-mcp: only build the library, skip examples and tests
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_MCP_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/cpp-mcp EXCLUDE_FROM_ALL)

# winpty(仅 WIN32 内部生效): Win10 < 1809 无 ConPTY 时的 PTY 兜底。
# 定义 winpty_static(链进 acecode_base_host)与 winpty-agent(嵌入释放)。
include(${CMAKE_SOURCE_DIR}/cmake/acecode_winpty.cmake)

# Gather all sources under src/ once, then split them into focused build
# boundaries. Desktop and daemon share a few native bridge helpers, but the
# desktop shell must not link the broad acecode_testable object set.
file(GLOB_RECURSE ACECODE_ALL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)
file(GLOB_RECURSE ACECODE_HEADER_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.hpp
    ${CMAKE_SOURCE_DIR}/src/*.h
)
file(GLOB ACECODE_THIRDPARTY_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/external/stb/*.h)
file(GLOB_RECURSE ACECODE_OBJCXX_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.mm)
acecode_assert_known_roots(SOURCES
    ${ACECODE_ALL_SOURCES} ${ACECODE_HEADER_FILES} ${ACECODE_OBJCXX_SOURCES})

# P3 M2(refactor20260927 D1):6 个分组各自作为 include 根,模块根形式的 include
# ("utils/paths.hpp")在分组搬迁后不变;generated 放版本头。
set(ACECODE_INCLUDE_ROOTS
    ${CMAKE_SOURCE_DIR}/src/base
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/adapters
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/src/host
    ${CMAKE_SOURCE_DIR}/src/apps
    ${CMAKE_BINARY_DIR}/generated)
foreach(_root IN LISTS ACECODE_INCLUDE_ROOTS)
    if(NOT _root MATCHES "generated$" AND NOT IS_DIRECTORY "${_root}")
        message(FATAL_ERROR "include root does not exist: ${_root}")
    endif()
endforeach()
add_library(acecode_include_roots INTERFACE)
target_include_directories(acecode_include_roots INTERFACE ${ACECODE_INCLUDE_ROOTS})
configure_file(cmake/acecode_tui_input_trace_config.hpp.in
    "${CMAKE_BINARY_DIR}/generated/acecode_tui_input_trace_config.hpp" @ONLY)

include(${CMAKE_SOURCE_DIR}/cmake/acecode_layer_libraries.cmake)

if(WIN32)
    # UI Automation lives outside the daemon: a hung application must remain
    # cancellable without terminating a conversation or the desktop shell.
    add_library(acecode_computer_use_native STATIC
        ${ACECODE_COMPUTER_USE_WINDOWS_SOURCES})
    target_link_libraries(acecode_computer_use_native PUBLIC acecode_base_core)
    target_include_directories(acecode_computer_use_native PUBLIC ${CMAKE_SOURCE_DIR}/src/adapters)
    target_compile_definitions(acecode_computer_use_native PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(acecode_computer_use_native PUBLIC
        nlohmann_json::nlohmann_json user32 gdi32 ole32 oleaut32
        uiautomationcore d3d11 dxgi windowscodecs dwmapi shell32 propsys uuid)
    if(MSVC)
        target_link_libraries(acecode_computer_use_native PUBLIC windowsapp)
    endif()
    add_executable(acecode-computer-use ${ACECODE_COMPUTER_USE_MAIN_SOURCE})
    target_link_libraries(acecode-computer-use PRIVATE acecode_computer_use_native)
    install(TARGETS acecode-computer-use RUNTIME DESTINATION . COMPONENT computer_use_runtime)
elseif(APPLE)
    enable_language(OBJCXX)
    add_library(acecode_computer_use_native STATIC
        ${ACECODE_COMPUTER_USE_MAC_SOURCES})
    set_target_properties(acecode_computer_use_native PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(acecode_computer_use_native PUBLIC acecode_base_core)
    target_include_directories(acecode_computer_use_native PUBLIC ${CMAKE_SOURCE_DIR}/src/adapters)
    target_compile_options(acecode_computer_use_native PRIVATE -fobjc-arc -mmacosx-version-min=14.0)
    target_link_libraries(acecode_computer_use_native PUBLIC nlohmann_json::nlohmann_json
        "-framework AppKit" "-framework ApplicationServices" "-framework ScreenCaptureKit"
        "-framework ImageIO" "-framework Carbon")
    add_executable(acecode-computer-use
        ${ACECODE_COMPUTER_USE_MAIN_SOURCE} ${ACECODE_COMPUTER_USE_MAC_MAIN_SOURCE})
    set_target_properties(acecode-computer-use PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
    target_compile_options(acecode-computer-use PRIVATE -mmacosx-version-min=14.0 "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_options(acecode-computer-use PRIVATE -mmacosx-version-min=14.0)
    target_link_libraries(acecode-computer-use PRIVATE acecode_computer_use_native)
    install(TARGETS acecode-computer-use RUNTIME DESTINATION . COMPONENT computer_use_runtime)
endif()

add_executable(acecode
    ${ACECODE_MAIN_SOURCE}
    ${ACECODE_WINDOWS_RESOURCES}
)
target_link_libraries(acecode PRIVATE acecode_cli)
if(WIN32 OR APPLE)
    add_dependencies(acecode acecode-computer-use)
endif()

add_custom_command(TARGET acecode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:acecode>/channels/whatsapp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/bridge.mjs"
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/protocol.mjs"
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/package.json"
        "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/package-lock.json"
        "$<TARGET_FILE_DIR:acecode>/channels/whatsapp"
    VERBATIM)

install(FILES
    "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/bridge.mjs"
    "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/protocol.mjs"
    "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/package.json"
    "${CMAKE_SOURCE_DIR}/assets/channels/whatsapp/package-lock.json"
    DESTINATION channels/whatsapp COMPONENT whatsapp_channel)
source_group(TREE ${CMAKE_SOURCE_DIR} FILES
    ${ACECODE_MAIN_SOURCE}
    ${ACECODE_ALL_SOURCES}
    ${ACECODE_HEADER_FILES}
    ${ACECODE_THIRDPARTY_HEADERS}
    ${CMAKE_SOURCE_DIR}/cmake/version.hpp.in
)

source_group("generated" FILES
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    ${ACECODE_WINDOWS_RESOURCES}
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# desktop shell 子目标(opt-in,默认 OFF)。include 后只有当 ACECODE_BUILD_DESKTOP=ON
# 才会真添加 acecode-desktop 这个 target,否则文件本身做无操作 return。
include(${CMAKE_SOURCE_DIR}/cmake/acecode_desktop.cmake)
if((WIN32 OR APPLE) AND TARGET acecode-desktop)
    add_dependencies(acecode-desktop acecode-computer-use)
endif()

# tuitest helloworld 子目标(opt-in,默认 OFF)。-DACECODE_BUILD_TUITEST=ON 开启。
include(${CMAKE_SOURCE_DIR}/cmake/acecode_tuitest.cmake)

include(${CMAKE_SOURCE_DIR}/cmake/acecode_layer_contract.cmake)

# Bundled models.dev snapshot. Installed alongside the executable so
# find_models_dev_dir() can resolve <exe_dir>/../share/acecode/models_dev.
if(EXISTS ${CMAKE_SOURCE_DIR}/assets/models_dev/api.json)
    install(FILES
        ${CMAKE_SOURCE_DIR}/assets/models_dev/api.json
        DESTINATION share/acecode/models_dev
        COMPONENT models_dev_registry
    )
    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/models_dev/MANIFEST.json)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/models_dev/MANIFEST.json
            DESTINATION share/acecode/models_dev
            COMPONENT models_dev_registry)
    endif()
    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/models_dev/LICENSE)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/models_dev/LICENSE
            DESTINATION share/acecode/models_dev
            COMPONENT models_dev_registry)
    endif()
endif()

# Versioned default Skill, expert, and managed hook seed bundle. Installed alongside the executable so
# find_default_skill_seed_dir() can resolve <exe_dir>/../share/acecode/seed/skills.
if(EXISTS ${CMAKE_SOURCE_DIR}/assets/seed/skills)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/seed/
        DESTINATION share/acecode/seed
        COMPONENT default_seed_bundle
    )
endif()
