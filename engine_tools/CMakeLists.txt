shark_setup_source_document_dependencies()
add_library(EngineTools STATIC src/build_info.cpp src/source_document.cpp src/authored_scene.cpp
    src/project_files.cpp src/project_files_native.cpp src/asset_catalog.cpp src/asset_cache_format.cpp src/asset_import.cpp
    src/scene_runner.cpp src/feature_world.cpp)
target_include_directories(EngineTools PUBLIC include PRIVATE "${PROJECT_SOURCE_DIR}/engine/core/src")
target_link_libraries(EngineTools PUBLIC EngineCore PRIVATE nlohmann_json::nlohmann_json)
shark_configure_target(EngineTools)
shark_classify_target(EngineTools authoring)

add_executable(SceneRunner src/scene_runner_main.cpp)
target_link_libraries(SceneRunner PRIVATE EngineTools)
shark_configure_target(SceneRunner)
shark_classify_target(SceneRunner authoring)
