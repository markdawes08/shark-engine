shark_setup_source_document_dependencies()
shark_setup_gltf_dependencies()
shark_setup_image_dependencies()
add_library(EngineTools STATIC src/build_info.cpp src/source_document.cpp src/authored_scene.cpp
    src/project_files.cpp src/project_files_native.cpp src/asset_catalog.cpp src/asset_cache_format.cpp src/asset_import.cpp
    src/scene_runner.cpp src/feature_world.cpp src/gltf_source.cpp src/gltf_image_decoder.cpp)
target_include_directories(EngineTools PUBLIC include PRIVATE "${PROJECT_SOURCE_DIR}/engine/core/src")
target_link_libraries(EngineTools PUBLIC EngineCore PRIVATE nlohmann_json::nlohmann_json Shark::Cgltf Shark::StbImage)
shark_configure_target(EngineTools)
# The private C++ parser allocator aborts on exhaustion before cgltf's unsafe
# partial-allocation cleanup. Permit unwinding through its C-linkage functions;
# retain synchronous exception handling and every other target's existing model.
if(MSVC)
    set_source_files_properties(src/gltf_source.cpp PROPERTIES COMPILE_OPTIONS "/EHc-")
endif()
shark_classify_target(EngineTools authoring)

add_executable(SceneRunner src/scene_runner_main.cpp)
target_link_libraries(SceneRunner PRIVATE EngineTools)
shark_configure_target(SceneRunner)
shark_classify_target(SceneRunner authoring)
