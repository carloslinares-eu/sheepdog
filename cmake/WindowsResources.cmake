# sheepdog_add_windows_resources(<target> <description>): embeds the version
# resource and the application manifest (long paths, UTF-8 code page,
# per-monitor DPI) into a Windows executable. No-op on other systems.
function(sheepdog_add_windows_resources target description)
    if(NOT WIN32)
        return()
    endif()
    set(SHEEPDOG_RC_NAME "${target}")
    set(SHEEPDOG_RC_DESCRIPTION "${description}")
    set(SHEEPDOG_RC_MANIFEST "${PROJECT_SOURCE_DIR}/cmake/windows/sheepdog.manifest")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${PROJECT_SOURCE_DIR}/cmake/windows/version.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()
