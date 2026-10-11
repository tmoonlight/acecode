# Generate deterministic gzip resources. Track both added/removed paths and
# modifications of existing files so an incremental native build refreshes Vite output.
set(_ACECODE_EMBED_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/../scripts/embed_web_assets.py")
function(acecode_generate_embedded_assets web_dir out_file version)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB_RECURSE assets CONFIGURE_DEPENDS "${web_dir}/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${assets} "${_ACECODE_EMBED_GENERATOR}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_ACECODE_EMBED_GENERATOR}"
            --root "${web_dir}" --output "${out_file}" --version "${version}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE report
        ERROR_VARIABLE errors
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Embedded asset generation failed: ${errors}")
    endif()
    string(STRIP "${report}" report)
    message(STATUS "${report}")
endfunction()

# 拿 git short hash 作 BUILD_VERSION;非 git 环境 fallback 用日期+秒。
function(acecode_compute_build_version out_var)
    set(_version "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE git_hash
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE rc
        )
        if(rc EQUAL 0 AND git_hash)
            set(_version "${git_hash}")
        endif()
    endif()
    if(_version STREQUAL "unknown")
        string(TIMESTAMP _version "%Y%m%d%H%M")
    endif()
    set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()
