# P5: each layer owns its translation units and publishes its include root.
# Specialized executables are excluded explicitly; the final contract checks
# that every active project source still has exactly one primary owner.
function(acecode_collect_group_sources output prefix)
    set(_sources)
    foreach(_source IN LISTS ACECODE_ALL_SOURCES ACECODE_HEADER_FILES)
        string(FIND "${_source}" "${CMAKE_SOURCE_DIR}/src/${prefix}/" _position)
        if(_position EQUAL 0)
            list(APPEND _sources "${_source}")
        endif()
    endforeach()
    set(${output} "${_sources}" PARENT_SCOPE)
endfunction()

function(acecode_add_layer_library target group)
    acecode_require_sources("${target}" ${ARGN})
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC
        "${CMAKE_SOURCE_DIR}/src/${group}"
        "${CMAKE_BINARY_DIR}/generated")
    set_target_properties(${target} PROPERTIES
        OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
endfunction()

set(ACECODE_LAYER_TARGETS
    acecode_base_core acecode_base_host acecode_domain acecode_adapters
    acecode_engine acecode_host acecode_web acecode_tui acecode_headless
    acecode_daemon acecode_cli acecode_desktop_support)
set(ACECODE_INACTIVE_SOURCES)

acecode_collect_group_sources(_base_core base)
set(_base_host)
foreach(_module IN ITEMS network environment pty)
    acecode_collect_group_sources(_module_sources "base/${_module}")
    list(APPEND _base_host ${_module_sources})
endforeach()
list(REMOVE_ITEM _base_core ${_base_host})
# This replacement implementation needs WinPTY's private upstream headers.
list(REMOVE_ITEM _base_host ${ACECODE_WINPTY_AGENT_LOCATION_SOURCE})
if(NOT WIN32)
    list(APPEND ACECODE_INACTIVE_SOURCES ${ACECODE_WINPTY_AGENT_LOCATION_SOURCE})
endif()

set(_notifications_win "${CMAKE_SOURCE_DIR}/src/base/platform/native_ui/notifications_win.cpp")
set(_notifications_stub "${CMAKE_SOURCE_DIR}/src/base/platform/native_ui/notifications_stub.cpp")
if(WIN32)
    list(REMOVE_ITEM _base_core "${_notifications_stub}")
    list(APPEND ACECODE_INACTIVE_SOURCES "${_notifications_stub}")
elseif(APPLE)
    list(REMOVE_ITEM _base_core "${_notifications_win}" "${_notifications_stub}")
    list(APPEND ACECODE_INACTIVE_SOURCES "${_notifications_win}" "${_notifications_stub}")
else()
    list(REMOVE_ITEM _base_core "${_notifications_win}")
    list(APPEND ACECODE_INACTIVE_SOURCES "${_notifications_win}")
endif()

acecode_add_layer_library(acecode_base_core base ${_base_core}
    ${ACECODE_THIRDPARTY_HEADERS} "${CMAKE_BINARY_DIR}/generated/version.hpp")
target_include_directories(acecode_base_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external")
target_link_libraries(acecode_base_core
    PUBLIC nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE unofficial::sqlite3::sqlite3 ${CMAKE_DL_LIBS})
if(APPLE)
    enable_language(OBJCXX)
    target_sources(acecode_base_core PRIVATE ${ACECODE_NATIVE_BRIDGE_MAC_SOURCES})
    target_link_libraries(acecode_base_core PRIVATE
        "-framework AppKit" "-framework UserNotifications")
elseif(WIN32)
    target_link_libraries(acecode_base_core PRIVATE
        bcrypt advapi32 ws2_32 ole32 shell32 user32 gdi32 msimg32 winmm)
endif()
if(NOT WIN32)
    # platform/crypto 的 AES-256-GCM 在非 Windows 平台走 libcrypto。
    target_link_libraries(acecode_base_core PRIVATE OpenSSL::Crypto)
endif()

acecode_add_layer_library(acecode_base_host base ${_base_host})
# network/websocket_client 直接使用 libcurl 的 WebSocket API。
target_link_libraries(acecode_base_host PUBLIC acecode_base_core cpr::cpr CURL::libcurl
    PRIVATE unofficial::sqlite3::sqlite3)
if(WIN32)
    target_link_libraries(acecode_base_host PRIVATE winhttp iphlpapi winpty_static)
elseif(UNIX AND NOT APPLE)
    # Older glibc/musl and ARMv7 cross sysroots still require libutil.
    find_library(ACECODE_LIBUTIL util)
    if(ACECODE_LIBUTIL)
        target_link_libraries(acecode_base_host PRIVATE ${ACECODE_LIBUTIL})
    elseif(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7")
        target_link_libraries(acecode_base_host PRIVATE util)
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 4
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7"
       AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        target_link_libraries(acecode_base_core PUBLIC atomic)
    endif()
endif()

acecode_collect_group_sources(_domain domain)
acecode_add_layer_library(acecode_domain domain ${_domain})
target_link_libraries(acecode_domain PUBLIC acecode_base_core
    PRIVATE unofficial::sqlite3::sqlite3)

acecode_collect_group_sources(_adapters adapters)
list(REMOVE_ITEM _adapters ${ACECODE_COMPUTER_USE_MAIN_SOURCE}
    ${ACECODE_COMPUTER_USE_WINDOWS_SOURCES})
if(NOT WIN32)
    list(APPEND ACECODE_INACTIVE_SOURCES ${ACECODE_COMPUTER_USE_WINDOWS_SOURCES})
endif()
if(NOT WIN32 AND NOT APPLE)
    list(APPEND ACECODE_INACTIVE_SOURCES ${ACECODE_COMPUTER_USE_MAIN_SOURCE})
endif()
acecode_add_layer_library(acecode_adapters adapters ${_adapters})
target_link_libraries(acecode_adapters PUBLIC acecode_domain acecode_base_host
    PRIVATE libzip::zip mcp)
target_include_directories(acecode_adapters PRIVATE
    "${CMAKE_SOURCE_DIR}/external/cpp-mcp/include"
    "${CMAKE_SOURCE_DIR}/external/cpp-mcp/common")
if(APPLE)
    target_sources(acecode_adapters PRIVATE ${ACECODE_UPGRADE_MAC_SOURCE})
    target_link_libraries(acecode_adapters PRIVATE
        "-framework Foundation" "-framework Security")
endif()

acecode_collect_group_sources(_engine engine)
acecode_add_layer_library(acecode_engine engine ${_engine})
target_link_libraries(acecode_engine PUBLIC acecode_adapters)

# Crow requires the same Asio mode even when asio.hpp is included first.
add_library(acecode_crow INTERFACE)
target_link_libraries(acecode_crow INTERFACE Crow::Crow)
target_compile_definitions(acecode_crow INTERFACE ASIO_STANDALONE)
if(WIN32)
    target_link_libraries(acecode_crow INTERFACE mswsock)
endif()

acecode_collect_group_sources(_host host)
acecode_add_layer_library(acecode_host host ${_host})
target_link_libraries(acecode_host PUBLIC acecode_engine PRIVATE acecode_crow)
acecode_set_source_define("${ACECODE_CHANNEL_BRIDGE_SOURCE}"
    ACECODE_CHANNEL_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/channels/whatsapp")

acecode_collect_group_sources(_web apps/web)
acecode_add_layer_library(acecode_web apps ${_web}
    "${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp")
target_link_libraries(acecode_web PUBLIC acecode_host acecode_crow)

acecode_collect_group_sources(_tui apps/tui)
acecode_add_layer_library(acecode_tui apps ${_tui}
    "${CMAKE_BINARY_DIR}/generated/acecode_tui_input_trace_config.hpp")
target_link_libraries(acecode_tui PUBLIC acecode_host
    ftxui::screen ftxui::dom ftxui::component)

acecode_collect_group_sources(_headless apps/headless)
acecode_add_layer_library(acecode_headless apps ${_headless})
target_link_libraries(acecode_headless PUBLIC acecode_host)

acecode_collect_group_sources(_daemon apps/daemon)
acecode_add_layer_library(acecode_daemon apps ${_daemon})
target_link_libraries(acecode_daemon PUBLIC acecode_web)

acecode_collect_group_sources(_cli apps/cli)
list(REMOVE_ITEM _cli ${ACECODE_MAIN_SOURCE})
acecode_add_layer_library(acecode_cli apps ${_cli})
target_link_libraries(acecode_cli PUBLIC acecode_tui acecode_headless acecode_daemon)

# Reusable desktop code is application-owned, with only base-layer linkage.
# Browser hosts and platform shell entry points remain executable-owned.
set(ACECODE_DESKTOP_BINARY_ONLY_SOURCES
    ${ACECODE_DEEPIN_WINDOW_EFFECTS_SOURCE} ${ACECODE_DESKTOP_LINUX_SOURCE}
    ${ACECODE_AGENT_BROWSER_HOST_SOURCE} ${ACECODE_AGENT_BROWSER_HOST_MAC_SOURCE}
    ${ACECODE_DESKTOP_MAIN_SOURCE} ${ACECODE_DESKTOP_SPLASH_SOURCE}
    ${ACECODE_DESKTOP_WEB_HOST_SOURCE} ${ACECODE_DESKTOP_PET_SOURCE}
    ${ACECODE_DESKTOP_PET_MAC_SOURCE})
acecode_collect_group_sources(_desktop apps/desktop)
list(REMOVE_ITEM _desktop ${ACECODE_DESKTOP_BINARY_ONLY_SOURCES})
acecode_add_layer_library(acecode_desktop_support apps ${_desktop})
target_link_libraries(acecode_desktop_support PUBLIC acecode_base_core)
if(APPLE)
    set_source_files_properties(${ACECODE_DESKTOP_OBJCXX_SOURCES} PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(${ACECODE_DESKTOP_WEB_HOST_SOURCE}
        PROPERTIES COMPILE_OPTIONS "-Wno-auto-var-id")
    target_link_libraries(acecode_desktop_support PRIVATE "-framework AppKit")
elseif(UNIX)
    target_link_libraries(acecode_desktop_support PRIVATE ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    target_link_libraries(acecode_desktop_support PRIVATE
        bcrypt ws2_32 ole32 shell32 user32 gdi32 gdiplus comctl32)
endif()

# The test aggregate has no sources or objects. Tests consume the production
# archives, including all TUI implementations, without a second selection list.
add_library(acecode_testable INTERFACE)
target_link_libraries(acecode_testable INTERFACE acecode_cli acecode_desktop_support)
