# sheepdog_core: the Qt-free engine. Nothing in here may include Qt headers.
add_library(sheepdog_core STATIC
    version.hpp
    platform/result.hpp
    platform/native_path.hpp
    platform/native_path.cpp
    platform/fs.hpp
    platform/fs_common.cpp
    "${SHEEPDOG_VERSION_CPP}")
add_library(sheepdog::core ALIAS sheepdog_core)
add_dependencies(sheepdog_core sheepdog_version_info)

# The platform layer: one implementation per OS, selected here.
if(WIN32)
    target_sources(sheepdog_core PRIVATE
        platform/win_util.hpp
        platform/path_win.cpp
        platform/fs_win.cpp
        platform/volume_win.cpp)
    target_compile_definitions(sheepdog_core PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
else()
    target_sources(sheepdog_core PRIVATE
        platform/fs_posix.cpp
        platform/volume_posix.cpp)
endif()

target_include_directories(sheepdog_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(sheepdog_core PUBLIC cxx_std_20)
sheepdog_set_warnings(sheepdog_core)
