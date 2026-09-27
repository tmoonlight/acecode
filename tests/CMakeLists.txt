find_package(GTest CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
    add_test(
        NAME verify_package_python_unit
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_package_unit_test.py
    )
    add_test(
        NAME dev_desktop_script_unit
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/dev_desktop_test.py
    )
    set_tests_properties(
        verify_package_python_unit
        dev_desktop_script_unit
        PROPERTIES LABELS "unit"
    )
    add_test(
        NAME macos_portable_package_unit
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/macos_portable_package_test.py
    )
    set_tests_properties(macos_portable_package_unit PROPERTIES LABELS "unit")
endif()

if(UNIX)
    add_test(
        NAME macos_release_scripts_contract
        COMMAND /bin/bash
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/macos_release_scripts_test.sh
    )
    set_tests_properties(macos_release_scripts_contract PROPERTIES LABELS "unit")

    add_test(
        NAME macos_pkg_installation_contract
        COMMAND /bin/bash
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/macos_pkg_installation_test.sh
    )
    set_tests_properties(macos_pkg_installation_contract PROPERTIES LABELS "unit")

    add_test(
        NAME verify_package_contract
        COMMAND /bin/bash
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_package_test.sh
    )
    set_tests_properties(verify_package_contract PROPERTIES LABELS "unit")
endif()

if(UNIX AND NOT APPLE)
    add_test(
        NAME linux_update_zip_contract
        COMMAND /bin/bash
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux_update_zip_test.sh
    )
    set_tests_properties(linux_update_zip_contract PROPERTIES LABELS "unit")
endif()

file(GLOB_RECURSE ACECODE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp
)

add_executable(acecode_unit_tests ${ACECODE_TEST_SOURCES})

add_executable(acecode_runtime_log_fixture
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/helpers/runtime_log_fixture.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/append_file.cpp)
target_include_directories(acecode_runtime_log_fixture PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external/ftxui/src)
if(MSVC)
    set_target_properties(acecode_runtime_log_fixture PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
    target_link_options(acecode_runtime_log_fixture PRIVATE -municode)
endif()
add_dependencies(acecode_unit_tests acecode_runtime_log_fixture)
if(Python3_Interpreter_FOUND)
    add_test(NAME runtime_log_append
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/runtime_log_append_test.py
            $<TARGET_FILE:acecode_runtime_log_fixture>)
    set_tests_properties(runtime_log_append PROPERTIES LABELS "unit" TIMEOUT 120)
endif()

add_executable(acecode_upgrade_version_fixture
    ${CMAKE_CURRENT_SOURCE_DIR}/upgrade/helpers/version_fixture.cpp)
if(MSVC)
    set_target_properties(acecode_upgrade_version_fixture PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_dependencies(acecode_unit_tests acecode_upgrade_version_fixture)
acecode_set_source_define(${CMAKE_CURRENT_SOURCE_DIR}/upgrade/executable_version_test.cpp
    "ACECODE_UPGRADE_VERSION_FIXTURE=\"$<TARGET_FILE:acecode_upgrade_version_fixture>\"")

add_executable(acecode_upgrade_restart_smoke EXCLUDE_FROM_ALL
    ${ACECODE_UPGRADE_RESTART_SMOKE_SOURCES})
target_link_libraries(acecode_upgrade_restart_smoke PRIVATE
    acecode_desktop_support acecode_native_bridge_support cpr::cpr nlohmann_json::nlohmann_json)
if(MSVC)
    set_target_properties(acecode_upgrade_restart_smoke PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(acecode_unit_tests PRIVATE
    acecode_testable
    acecode_desktop_support
    acecode_native_bridge_support
    ftxui::screen
    ftxui::dom
    ftxui::component
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(acecode_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_BINARY_DIR}/generated
)
if(WIN32 OR APPLE)
    target_link_libraries(acecode_unit_tests PRIVATE acecode_computer_use_native)
    add_dependencies(acecode_unit_tests acecode-computer-use)
    # Runtime tests resolve their sidecar beside the test executable as the
    # production daemon does, including multi-configuration generators.
    add_custom_command(TARGET acecode_unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:acecode-computer-use>
            $<TARGET_FILE_DIR:acecode_unit_tests>/$<TARGET_FILE_NAME:acecode-computer-use>
        VERBATIM)
    # Isolated transport tests deliberately substitute a noninteractive worker.
    # Keep its executable directory separate from the real helper and tests.
    set(_computer_broker_dir "${CMAKE_CURRENT_BINARY_DIR}/computer-use-broker/$<CONFIG>")
    add_executable(computer_use_broker_fixture EXCLUDE_FROM_ALL
        ${ACECODE_COMPUTER_USE_BROKER_FIXTURE_SOURCES})
    target_link_libraries(computer_use_broker_fixture PRIVATE nlohmann_json::nlohmann_json)
    set_target_properties(computer_use_broker_fixture PROPERTIES
        OUTPUT_NAME acecode-computer-use RUNTIME_OUTPUT_DIRECTORY "${_computer_broker_dir}")
    add_executable(computer_use_broker_smoke EXCLUDE_FROM_ALL
        ${ACECODE_COMPUTER_USE_BROKER_SMOKE_SOURCES})
    target_include_directories(computer_use_broker_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(computer_use_broker_smoke PRIVATE nlohmann_json::nlohmann_json)
    set_target_properties(computer_use_broker_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_computer_broker_dir}")
    add_dependencies(computer_use_broker_smoke computer_use_broker_fixture)
endif()
if(WIN32)
    # Opt-in native fixture: operates only on its own disposable test window.
    add_executable(computer_use_native_smoke EXCLUDE_FROM_ALL
        ${ACECODE_COMPUTER_USE_NATIVE_SMOKE_SOURCES})
    target_link_libraries(computer_use_native_smoke PRIVATE acecode_computer_use_native)
    add_dependencies(computer_use_native_smoke acecode-computer-use)
    add_custom_command(TARGET computer_use_native_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:acecode-computer-use>
            $<TARGET_FILE_DIR:computer_use_native_smoke>/acecode-computer-use.exe
        VERBATIM)
endif()

# Manual, opt-in Windows runtime smoke. It owns an isolated top-level HWND and
# proves that the native Browser host proxies CDP to the exact visible WebView2
# page without an external debug port or ACECode host bindings in web content.
if(WIN32 AND ACECODE_BUILD_DESKTOP)
    add_executable(agent_browser_host_smoke EXCLUDE_FROM_ALL
        ${ACECODE_AGENT_BROWSER_HOST_SMOKE_SOURCES}
    )
    target_include_directories(agent_browser_host_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(agent_browser_host_smoke PRIVATE
        acecode_desktop_support
        acecode_native_bridge_support
        webview::core
        cpr::cpr
        nlohmann_json::nlohmann_json
        ole32 user32 ws2_32
    )
    if(MSVC)
        set_target_properties(agent_browser_host_smoke PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    # Manual visual/runtime demo for the exact AI pointer overlay used by
    # mouse tools and synthetic events from browser_evaluate. It runs in an
    # isolated native WebView2 host so it never takes over the user's active
    # Browser tab.
    add_executable(agent_browser_pointer_demo EXCLUDE_FROM_ALL
        ${ACECODE_AGENT_BROWSER_POINTER_DEMO_SOURCES}
    )
    target_include_directories(agent_browser_pointer_demo PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(agent_browser_pointer_demo PRIVATE
        acecode_desktop_support
        acecode_native_bridge_support
        webview::core
        cpr::cpr
        nlohmann_json::nlohmann_json
        ole32 user32 ws2_32
    )
    if(MSVC)
        set_target_properties(agent_browser_pointer_demo PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()

if(APPLE)
    add_executable(computer_use_native_mac_smoke MACOSX_BUNDLE EXCLUDE_FROM_ALL
        ${ACECODE_COMPUTER_USE_NATIVE_MAC_SMOKE_SOURCES})
    target_include_directories(computer_use_native_mac_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(computer_use_native_mac_smoke PRIVATE nlohmann_json::nlohmann_json
        "-framework AppKit" "-framework ApplicationServices")
    target_compile_options(computer_use_native_mac_smoke PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    set_target_properties(computer_use_native_mac_smoke PROPERTIES
        OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES
        MACOSX_BUNDLE_BUNDLE_NAME "ACECode Computer Use Fixture"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.acecode.computer-use-fixture")
    add_dependencies(computer_use_native_mac_smoke acecode-computer-use)
    set_property(TARGET computer_use_native_mac_smoke APPEND PROPERTY
        LINK_DEPENDS "$<TARGET_FILE:acecode-computer-use>")
    add_custom_command(TARGET computer_use_native_mac_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:acecode-computer-use>
            $<TARGET_FILE_DIR:computer_use_native_mac_smoke>/acecode-computer-use
        VERBATIM)
    if(Python3_Interpreter_FOUND)
        add_test(NAME computer_use_macos_helper_lifecycle
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/computer_use/macos_helper_test.py
                $<TARGET_FILE:acecode-computer-use>)
        set_tests_properties(computer_use_macos_helper_lifecycle PROPERTIES
            LABELS "unit" TIMEOUT 30 RUN_SERIAL TRUE)
    endif()
    add_executable(agent_browser_host_mac_smoke
        MACOSX_BUNDLE EXCLUDE_FROM_ALL
        ${ACECODE_AGENT_BROWSER_HOST_MAC_SMOKE_SOURCES}
    )
    target_link_libraries(agent_browser_host_mac_smoke PRIVATE
        acecode_desktop_support
        acecode_native_bridge_support
        nlohmann_json::nlohmann_json
        "-framework AppKit"
        "-framework ApplicationServices"
        "-framework CoreGraphics"
        "-framework QuartzCore"
        "-framework WebKit"
    )
    target_include_directories(agent_browser_host_mac_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/generated
    )
    set_target_properties(agent_browser_host_mac_smoke PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "ACECode Agent Browser Smoke"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.acecode.agent-browser-smoke"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_SOURCE_DIR}/cmake/macos/ACECodeDesktopInfo.plist.in"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ACECODE_BUILD_VERSION}"
    )

    # Manual, opt-in runtime check. Run the bundle executable directly so its
    # authorization state and click callback remain visible in the terminal.
    add_executable(acecode_macos_notification_smoke
        MACOSX_BUNDLE EXCLUDE_FROM_ALL
        ${ACECODE_MACOS_NOTIFICATION_SMOKE_SOURCES}
    )
    target_link_libraries(acecode_macos_notification_smoke PRIVATE
        acecode_native_bridge_support
    )
    target_include_directories(acecode_macos_notification_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    set_target_properties(acecode_macos_notification_smoke PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "ACECode Notification Smoke"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.acecode.notification-smoke"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_SOURCE_DIR}/cmake/macos/ACECodeDesktopInfo.plist.in"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ACECODE_BUILD_VERSION}"
    )
endif()

# 测试辅助 binary: 用于 SessionConcurrencyE2E 测试 std::system 起两个子进程
# 验证同 cwd 下多 session 写 canonical 文件。独立 main(),不参与
# gtest_discover_tests。链 acecode_testable 拿 SessionManager。
add_executable(concurrent_session_writer
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon/helpers/concurrent_session_writer.cpp
)
target_link_libraries(concurrent_session_writer PRIVATE
    acecode_testable
    acecode_native_bridge_support
    # acecode_testable 里 src/tool/ask_user_question_tool.cpp 等 TU 引用了
    # ftxui::Event,所以即使 helper main 自己只调 SessionManager,链接器也要
    # 拉 ftxui 才能 resolve。匹配 acecode_unit_tests 的 link 列表。
    ftxui::screen
    ftxui::dom
    ftxui::component
)
target_include_directories(concurrent_session_writer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
add_dependencies(acecode_unit_tests concurrent_session_writer)

# Runtime state is shared by multiple workspace daemons. This helper lets the
# state-file tests race two real processes against the first-start claim.
add_executable(state_file_claim_worker
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/helpers/state_file_claim_worker.cpp
)
target_link_libraries(state_file_claim_worker PRIVATE
    acecode_testable
    acecode_native_bridge_support
    ftxui::screen
    ftxui::dom
    ftxui::component
)
target_include_directories(state_file_claim_worker PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
add_dependencies(acecode_unit_tests state_file_claim_worker)

# Real child used by remote-Web proxy controller tests. It dispatches the same
# hidden command as the production executable without linking the TUI main.
add_executable(remote_web_proxy_test_child
    ${CMAKE_CURRENT_SOURCE_DIR}/web/helpers/remote_web_proxy_child.cpp
)
target_link_libraries(remote_web_proxy_test_child PRIVATE
    acecode_testable
    acecode_native_bridge_support
    ftxui::screen
    ftxui::dom
    ftxui::component
)
target_include_directories(remote_web_proxy_test_child PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
add_dependencies(acecode_unit_tests remote_web_proxy_test_child)

# 测试辅助 MCP stdio server: 用于 McpManager 异步启动测试走真实
# stdio JSON-RPC 路径,避免依赖外部 node/python。
add_executable(mcp_stdio_test_server
    ${CMAKE_CURRENT_SOURCE_DIR}/tool/helpers/mcp_stdio_test_server.cpp
)
target_link_libraries(mcp_stdio_test_server PRIVATE
    nlohmann_json::nlohmann_json
)
add_dependencies(acecode_unit_tests mcp_stdio_test_server)

# 测试辅助 LSP stdio server: LspClient 集成测试走真实进程/管道/
# Content-Length 分帧路径,不依赖外部 node / 真实 clangd。
add_executable(fake_lsp_server
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp/helpers/fake_lsp_server.cpp
)
target_link_libraries(fake_lsp_server PRIVATE
    nlohmann_json::nlohmann_json
)
add_dependencies(acecode_unit_tests fake_lsp_server)

# JSONL stdout process probe: links the production writer directly so tests can
# verify flush/backpressure/broken-pipe behavior through a real OS pipe without
# starting a provider-backed headless session.
add_executable(headless_jsonl_process_driver
    ${CMAKE_CURRENT_SOURCE_DIR}/headless/headless_jsonl_process_driver.cpp
    ${CMAKE_SOURCE_DIR}/src/headless/headless_jsonl.cpp
)
target_include_directories(headless_jsonl_process_driver PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(headless_jsonl_process_driver PRIVATE
    nlohmann_json::nlohmann_json
)
add_dependencies(acecode_unit_tests headless_jsonl_process_driver)

# 注入 helper 绝对路径给测试代码(避免 ctest cwd 不确定问题)
target_compile_definitions(acecode_unit_tests PRIVATE
    ACECODE_CONCURRENT_SESSION_WRITER_PATH="$<TARGET_FILE:concurrent_session_writer>"
    ACECODE_STATE_FILE_CLAIM_WORKER_PATH="$<TARGET_FILE:state_file_claim_worker>"
    ACECODE_REMOTE_WEB_PROXY_CHILD_PATH="$<TARGET_FILE:remote_web_proxy_test_child>"
    ACECODE_MCP_STDIO_TEST_SERVER_PATH="$<TARGET_FILE:mcp_stdio_test_server>"
    ACECODE_FAKE_LSP_SERVER_PATH="$<TARGET_FILE:fake_lsp_server>"
    ACECODE_HEADLESS_JSONL_DRIVER_PATH="$<TARGET_FILE:headless_jsonl_process_driver>"
)

# winpty spike/冒烟测试需要 winpty-agent.exe 在单测 exe 同目录
# (findAgentProgram 未设置覆盖路径时的 fallback 查找位置)。
if(WIN32)
    add_custom_command(TARGET acecode_unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:winpty-agent>
            $<TARGET_FILE_DIR:acecode_unit_tests>
    )
    add_dependencies(acecode_unit_tests winpty-agent)
endif()

include(GoogleTest)
gtest_discover_tests(acecode_unit_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)
