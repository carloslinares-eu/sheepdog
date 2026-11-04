add_executable(sheepdog-cli main.cpp)
target_link_libraries(sheepdog-cli PRIVATE sheepdog::core)
sheepdog_set_warnings(sheepdog-cli)
sheepdog_add_windows_resources(sheepdog-cli "Sheepdog command-line interface")
