find_package(Catch2 3 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
include(Catch)

# Engine tests: no Qt. Catch2 tags become ctest labels (unit fs gui fault slow win linux),
# so `ctest -L unit` picks a group.
add_executable(sheepdog_tests
    version_test.cpp
    dependencies_test.cpp
    platform_path_test.cpp
    platform_fs_test.cpp
    catalog_test.cpp
    catalog_writer_test.cpp
    jobs_progress_test.cpp
    jobs_pool_test.cpp
    jobs_job_test.cpp
    jobs_chunks_test.cpp
    support/ci.hpp
    support/jobs_support.hpp
    support/temp_tree.hpp
    support/temp_tree.cpp)
if(WIN32)
    target_sources(sheepdog_tests PRIVATE platform_win_test.cpp)
    target_compile_definitions(sheepdog_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
else()
    target_sources(sheepdog_tests PRIVATE platform_linux_test.cpp)
endif()
if(SHEEPDOG_SANITIZE)
    # Timing limits (the catalog writer's 2 s) are checked only in optimised, unsanitized builds.
    target_compile_definitions(sheepdog_tests PRIVATE SHEEPDOG_SANITIZED=1)
endif()
target_include_directories(sheepdog_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sheepdog_tests PRIVATE
    sheepdog::core Catch2::Catch2WithMain unofficial::sqlite3::sqlite3 BLAKE3::blake3)
sheepdog_set_warnings(sheepdog_tests)
catch_discover_tests(sheepdog_tests ADD_TAGS_AS_LABELS)

if(SHEEPDOG_BUILD_GUI)
    # Offscreen GUI tests. Discovery runs at test time, with Qt's DLLs on PATH on Windows.
    add_executable(sheepdog_gui_tests gui_main_window_test.cpp)
    target_link_libraries(sheepdog_gui_tests PRIVATE sheepdog_gui Catch2::Catch2WithMain)
    sheepdog_set_warnings(sheepdog_gui_tests)

    set(gui_env "QT_QPA_PLATFORM=offscreen")
    set(gui_dl_paths "")
    if(WIN32)
        set(gui_dl_paths DL_PATHS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    endif()
    catch_discover_tests(sheepdog_gui_tests
        ADD_TAGS_AS_LABELS
        DISCOVERY_MODE PRE_TEST
        ${gui_dl_paths}
        PROPERTIES ENVIRONMENT "${gui_env}")
endif()
