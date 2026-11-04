# sheepdog_core: the Qt-free engine. Nothing in here may include Qt headers.
add_library(sheepdog_core STATIC
    version.hpp
    "${SHEEPDOG_VERSION_CPP}")
add_library(sheepdog::core ALIAS sheepdog_core)
add_dependencies(sheepdog_core sheepdog_version_info)

target_include_directories(sheepdog_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(sheepdog_core PUBLIC cxx_std_20)
sheepdog_set_warnings(sheepdog_core)
