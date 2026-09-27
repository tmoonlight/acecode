# 被多个 target 或子目录引用的源码只登记一次，搬迁时不会遗漏独立冒烟目标。
set(ACECODE_MAIN_SOURCE "${CMAKE_SOURCE_DIR}/src/main.cpp")
set(ACECODE_UPGRADE_MANIFEST_SOURCE "${CMAKE_SOURCE_DIR}/src/upgrade/manifest.cpp")
set(ACECODE_CHANNEL_BRIDGE_SOURCE "${CMAKE_SOURCE_DIR}/src/channels/bridge.cpp")
set(ACECODE_WINPTY_AGENT_LOCATION_SOURCE "${CMAKE_SOURCE_DIR}/src/web/pty/winpty_agent_location.cpp")
set(ACECODE_DEEPIN_WINDOW_EFFECTS_SOURCE "${CMAKE_SOURCE_DIR}/src/desktop/deepin_window_effects.cpp")
set(ACECODE_DESKTOP_MAIN_SOURCE "${CMAKE_SOURCE_DIR}/src/desktop/main.cpp")
set(ACECODE_DESKTOP_SPLASH_SOURCE "${CMAKE_SOURCE_DIR}/src/desktop/splash_screen.cpp")
set(ACECODE_DESKTOP_WEB_HOST_SOURCE "${CMAKE_SOURCE_DIR}/src/desktop/web_host.cpp")
set(ACECODE_DESKTOP_LINUX_SOURCE "${CMAKE_SOURCE_DIR}/src/desktop/linux_desktop.cpp")
set(ACECODE_AGENT_BROWSER_HOST_SOURCE "${CMAKE_SOURCE_DIR}/src/desktop/agent_browser_host.cpp")
set(ACECODE_AGENT_BROWSER_HOST_MAC_SOURCE "${CMAKE_SOURCE_DIR}/src/desktop/agent_browser_host_mac.mm")
set(ACECODE_BROWSER_CDP_SOURCE "${CMAKE_SOURCE_DIR}/src/tool/agent_browser/cdp_client.cpp")
set(ACECODE_BROWSER_POINTER_SOURCE "${CMAKE_SOURCE_DIR}/src/tool/agent_browser/pointer_overlay.cpp")
set(ACECODE_COMPUTER_USE_RUNTIME_SOURCE "${CMAKE_SOURCE_DIR}/src/computer_use/runtime.cpp")
set(ACECODE_COMPUTER_USE_AVAILABILITY_SOURCE "${CMAKE_SOURCE_DIR}/src/computer_use/availability.cpp")
set(ACECODE_COMPUTER_USE_PROCESS_SOURCE "${CMAKE_SOURCE_DIR}/src/computer_use/helper_process_posix.cpp")
set(ACECODE_COMPUTER_USE_MAIN_SOURCE "${CMAKE_SOURCE_DIR}/src/computer_use/helper_main.cpp")
set(ACECODE_COMPUTER_USE_MAC_MAIN_SOURCE "${CMAKE_SOURCE_DIR}/src/computer_use/helper_main_macos.mm")
set(ACECODE_COMPUTER_USE_WINDOWS_SOURCES
    ${CMAKE_SOURCE_DIR}/src/computer_use/native_windows.cpp
    ${CMAKE_SOURCE_DIR}/src/computer_use/pointer_capture.cpp
    ${CMAKE_SOURCE_DIR}/src/computer_use/pointer_overlay.cpp)
set(ACECODE_COMPUTER_USE_MAC_SOURCES
    ${CMAKE_SOURCE_DIR}/src/computer_use/native_macos.mm
    ${CMAKE_SOURCE_DIR}/src/computer_use/macos_windows.mm
    ${CMAKE_SOURCE_DIR}/src/computer_use/macos_accessibility.mm
    ${CMAKE_SOURCE_DIR}/src/computer_use/macos_capture.mm
    ${CMAKE_SOURCE_DIR}/src/computer_use/macos_input.mm
    ${CMAKE_SOURCE_DIR}/src/computer_use/macos_pointer.mm)
set(ACECODE_NATIVE_BRIDGE_MAC_SOURCES
    ${CMAKE_SOURCE_DIR}/src/desktop/folder_picker_mac.mm
    ${CMAKE_SOURCE_DIR}/src/desktop/notifications_mac.mm)
set(ACECODE_DESKTOP_OBJCXX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/desktop/tray_icon_win.cpp
    ${ACECODE_DESKTOP_WEB_HOST_SOURCE})
set(ACECODE_UPGRADE_MAC_SOURCE "${CMAKE_SOURCE_DIR}/src/upgrade/macos_app_installer.mm")

set(ACECODE_UPGRADE_RESTART_SMOKE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/desktop/helpers/upgrade_restart_smoke.cpp)
set(ACECODE_COMPUTER_USE_BROKER_FIXTURE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/computer_use/helpers/broker_fixture.cpp)
set(ACECODE_COMPUTER_USE_BROKER_SMOKE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/computer_use/helpers/broker_smoke.cpp
    ${ACECODE_COMPUTER_USE_RUNTIME_SOURCE})
if(APPLE)
    list(APPEND ACECODE_COMPUTER_USE_BROKER_SMOKE_SOURCES
        ${ACECODE_COMPUTER_USE_PROCESS_SOURCE})
endif()
set(ACECODE_COMPUTER_USE_NATIVE_SMOKE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/computer_use/helpers/native_smoke.cpp
    ${CMAKE_SOURCE_DIR}/tests/computer_use/helpers/ole_drag_fixture.cpp
    ${ACECODE_COMPUTER_USE_RUNTIME_SOURCE})
set(ACECODE_COMPUTER_USE_NATIVE_MAC_SMOKE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/computer_use/helpers/native_macos_smoke.mm
    ${ACECODE_COMPUTER_USE_RUNTIME_SOURCE}
    ${ACECODE_COMPUTER_USE_AVAILABILITY_SOURCE}
    ${ACECODE_COMPUTER_USE_PROCESS_SOURCE})
set(ACECODE_AGENT_BROWSER_HOST_SMOKE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/desktop/helpers/agent_browser_host_smoke.cpp
    ${ACECODE_AGENT_BROWSER_HOST_SOURCE}
    ${ACECODE_BROWSER_CDP_SOURCE})
set(ACECODE_AGENT_BROWSER_POINTER_DEMO_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/desktop/helpers/agent_browser_pointer_demo.cpp
    ${ACECODE_AGENT_BROWSER_HOST_SOURCE}
    ${ACECODE_BROWSER_CDP_SOURCE}
    ${ACECODE_BROWSER_POINTER_SOURCE})
set(ACECODE_AGENT_BROWSER_HOST_MAC_SMOKE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/desktop/helpers/agent_browser_host_mac_smoke.mm
    ${ACECODE_AGENT_BROWSER_HOST_MAC_SOURCE}
    ${ACECODE_BROWSER_CDP_SOURCE})
set(ACECODE_MACOS_NOTIFICATION_SMOKE_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/desktop/helpers/macos_notification_smoke.mm)

# 不按当前平台跳过校验：在 Windows configure 时也能发现 .mm 路径拼错。
acecode_require_sources("shared target source paths"
    ${ACECODE_MAIN_SOURCE}
    ${ACECODE_UPGRADE_MANIFEST_SOURCE}
    ${ACECODE_CHANNEL_BRIDGE_SOURCE}
    ${ACECODE_WINPTY_AGENT_LOCATION_SOURCE}
    ${ACECODE_DEEPIN_WINDOW_EFFECTS_SOURCE}
    ${ACECODE_DESKTOP_MAIN_SOURCE}
    ${ACECODE_DESKTOP_SPLASH_SOURCE}
    ${ACECODE_DESKTOP_WEB_HOST_SOURCE}
    ${ACECODE_DESKTOP_LINUX_SOURCE}
    ${ACECODE_AGENT_BROWSER_HOST_SOURCE}
    ${ACECODE_AGENT_BROWSER_HOST_MAC_SOURCE}
    ${ACECODE_BROWSER_CDP_SOURCE}
    ${ACECODE_BROWSER_POINTER_SOURCE}
    ${ACECODE_COMPUTER_USE_RUNTIME_SOURCE}
    ${ACECODE_COMPUTER_USE_AVAILABILITY_SOURCE}
    ${ACECODE_COMPUTER_USE_PROCESS_SOURCE}
    ${ACECODE_COMPUTER_USE_MAIN_SOURCE}
    ${ACECODE_COMPUTER_USE_MAC_MAIN_SOURCE}
    ${ACECODE_COMPUTER_USE_WINDOWS_SOURCES}
    ${ACECODE_COMPUTER_USE_MAC_SOURCES}
    ${ACECODE_NATIVE_BRIDGE_MAC_SOURCES}
    ${ACECODE_DESKTOP_OBJCXX_SOURCES}
    ${ACECODE_UPGRADE_MAC_SOURCE})
if(BUILD_TESTING)
    acecode_require_sources("opt-in smoke source paths"
        ${ACECODE_UPGRADE_RESTART_SMOKE_SOURCES}
        ${ACECODE_COMPUTER_USE_BROKER_FIXTURE_SOURCES}
        ${ACECODE_COMPUTER_USE_BROKER_SMOKE_SOURCES}
        ${ACECODE_COMPUTER_USE_NATIVE_SMOKE_SOURCES}
        ${ACECODE_COMPUTER_USE_NATIVE_MAC_SMOKE_SOURCES}
        ${ACECODE_AGENT_BROWSER_HOST_SMOKE_SOURCES}
        ${ACECODE_AGENT_BROWSER_POINTER_DEMO_SOURCES}
        ${ACECODE_AGENT_BROWSER_HOST_MAC_SMOKE_SOURCES}
        ${ACECODE_MACOS_NOTIFICATION_SMOKE_SOURCES})
endif()
