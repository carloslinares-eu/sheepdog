# sheepdog_set_warnings(<target>): the project's warning flags.
# WARNINGS_AS_ERRORS=ON (the win-ci preset and CI) turns them into errors.
function(sheepdog_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            -Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
        if(WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
