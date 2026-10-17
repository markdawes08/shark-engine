add_executable(Editor main.cpp)
target_link_libraries(Editor PRIVATE EngineTools Engine)
shark_configure_target(Editor)
shark_classify_target(Editor authoring)
