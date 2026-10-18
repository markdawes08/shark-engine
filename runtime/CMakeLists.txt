add_executable(Runtime main.cpp)
target_link_libraries(Runtime PRIVATE Engine)
shark_configure_target(Runtime)
shark_classify_target(Runtime client)
