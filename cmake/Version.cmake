# Generates ${SHEEPDOG_VERSION_CPP} at every build from project(VERSION) plus
# `git describe`, so the version string follows new commits without a re-configure.
set(SHEEPDOG_VERSION_CPP "${PROJECT_BINARY_DIR}/generated/version_info.cpp")

add_custom_target(sheepdog_version_info
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DOUTPUT=${SHEEPDOG_VERSION_CPP}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -P "${PROJECT_SOURCE_DIR}/cmake/GitVersion.cmake"
    BYPRODUCTS "${SHEEPDOG_VERSION_CPP}"
    COMMENT "Updating version information"
    VERBATIM)
