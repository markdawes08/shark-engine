add_executable(Server main.cpp)
target_link_libraries(Server PRIVATE EngineCore)
shark_configure_target(Server)
shark_classify_target(Server core)
