# SHEEPDOG_SANITIZE (e.g. "address,undefined" or "thread") applies to every target.
if(SHEEPDOG_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SHEEPDOG_SANITIZE is only supported with GCC and Clang")
    endif()
    add_compile_options(-fsanitize=${SHEEPDOG_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${SHEEPDOG_SANITIZE})
endif()
