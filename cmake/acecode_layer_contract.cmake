# Evaluated after optional Desktop/Deepin targets have been created.
acecode_assert_layer_targets(TARGETS ${ACECODE_LAYER_TARGETS} AGGREGATE acecode_testable)
foreach(_layer IN LISTS ACECODE_LAYER_TARGETS)
    acecode_assert_no_link_path(${_layer} acecode_include_roots)
endforeach()
set(_primary_targets ${ACECODE_LAYER_TARGETS} acecode)
set(_desktop_inactive ${ACECODE_DESKTOP_BINARY_ONLY_SOURCES})
if(TARGET acecode-desktop)
    list(APPEND _primary_targets acecode-desktop)
    get_target_property(_desktop_sources acecode-desktop SOURCES)
    list(REMOVE_ITEM _desktop_inactive ${_desktop_sources})
    acecode_assert_no_link_path(acecode-desktop
        acecode_testable acecode_include_roots acecode_base_host acecode_domain acecode_adapters
        acecode_engine acecode_host acecode_web acecode_tui acecode_headless
        acecode_daemon acecode_cli acecode_crow Crow::Crow mcp)
endif()
if(TARGET acecode_deepin_window_effects)
    list(APPEND _primary_targets acecode_deepin_window_effects)
    list(REMOVE_ITEM _desktop_inactive ${ACECODE_DEEPIN_WINDOW_EFFECTS_SOURCE})
endif()
if(TARGET acecode_computer_use_native)
    list(APPEND _primary_targets acecode_computer_use_native acecode-computer-use)
endif()
if(TARGET winpty_static)
    list(APPEND _primary_targets winpty_static)
endif()
list(APPEND ACECODE_INACTIVE_SOURCES ${_desktop_inactive})
if(NOT APPLE)
    list(APPEND ACECODE_INACTIVE_SOURCES ${ACECODE_OBJCXX_SOURCES})
endif()
list(REMOVE_DUPLICATES ACECODE_INACTIVE_SOURCES)
acecode_assert_primary_sources(
    SOURCES ${ACECODE_ALL_SOURCES} ${ACECODE_OBJCXX_SOURCES}
    TARGETS ${_primary_targets}
    INACTIVE ${ACECODE_INACTIVE_SOURCES})
acecode_assert_no_link_path(acecode_domain
    acecode_base_host acecode_adapters acecode_engine acecode_host
    acecode_web acecode_tui acecode_crow Crow::Crow cpr::cpr mcp)
