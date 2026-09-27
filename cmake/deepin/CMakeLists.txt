find_package(Qt5 5.11 REQUIRED COMPONENTS Widgets)
find_package(X11 REQUIRED)
find_package(DtkWidget REQUIRED)
if(TARGET Dtk::Widget)
    set(_acecode_dtk_target Dtk::Widget)
else()
    # DTK 5.2 predates the imported Dtk::Widget CMake target.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ACECODE_DTK REQUIRED IMPORTED_TARGET
        dtkwidget>=5.2 dtkgui>=5.2 dtkcore>=5.2)
    set(_acecode_dtk_target PkgConfig::ACECODE_DTK)
endif()

add_library(acecode_deepin_window_effects STATIC
    ${ACECODE_DEEPIN_WINDOW_EFFECTS_SOURCE})
target_compile_definitions(acecode_deepin_window_effects PRIVATE
    ACECODE_DEEPIN=1 QT_NO_KEYWORDS)
target_include_directories(acecode_deepin_window_effects PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(acecode_deepin_window_effects PRIVATE
    ${_acecode_dtk_target} Qt5::Widgets X11::X11 webview::core)
