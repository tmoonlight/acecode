cmake_minimum_required(VERSION 3.20)
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "" FORCE)
project(acecode VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force UTF-8 source and execution charset on MSVC
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Generate version header from CMake project version
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

find_package(cpr CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB_RECURSE ACECODE_SOURCE_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

file(GLOB_RECURSE ACECODE_HEADER_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.hpp
    ${CMAKE_SOURCE_DIR}/src/*.h
)

set(ACECODE_PROJECT_FILES
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${ACECODE_SOURCE_FILES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
)

add_executable(acecode)
target_sources(acecode PRIVATE ${ACECODE_PROJECT_FILES})

source_group(TREE ${CMAKE_SOURCE_DIR} FILES
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${ACECODE_SOURCE_FILES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
)

source_group("generated" FILES
    ${CMAKE_BINARY_DIR}/generated/version.hpp
)

target_include_directories(acecode PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(acecode PRIVATE 
    cpr::cpr 
    ftxui::screen 
    ftxui::dom 
    ftxui::component
    nlohmann_json::nlohmann_json
)

