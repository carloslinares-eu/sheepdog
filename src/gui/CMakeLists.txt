find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# The GUI code lives in a library so the offscreen tests can link it.
add_library(sheepdog_gui STATIC
    main_window.hpp
    main_window.cpp)
target_link_libraries(sheepdog_gui PUBLIC sheepdog::core Qt6::Widgets)
target_include_directories(sheepdog_gui PUBLIC "${PROJECT_SOURCE_DIR}/src")
sheepdog_set_warnings(sheepdog_gui)

qt_add_executable(sheepdog WIN32 main.cpp)
target_link_libraries(sheepdog PRIVATE sheepdog_gui)
sheepdog_set_warnings(sheepdog)
sheepdog_add_windows_resources(sheepdog "Sheepdog")
