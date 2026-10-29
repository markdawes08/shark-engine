shark_setup_source_document_dependencies()
shark_setup_gltf_dependencies()
shark_setup_image_dependencies()
add_library(EngineTools STATIC src/build_info.cpp src/source_document.cpp src/authored_scene.cpp src/transform_manipulation.cpp
    src/project_files.cpp src/project_files_native.cpp src/asset_catalog.cpp src/asset_cache_format.cpp src/asset_import.cpp
    src/scene_runner.cpp src/feature_world.cpp src/gltf_source.cpp src/gltf_image_decoder.cpp
    src/gltf_asset.cpp src/prepared_content.cpp src/client_export.cpp
    src/authoring_protocol.cpp src/authoring_wire.cpp src/authoring_assets.cpp src/authoring_asset_wire.cpp
    src/authoring_run.cpp src/authoring_execution_wire.cpp src/authoring_export.cpp src/export_content_stage.cpp)
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

add_executable(ExportContent src/client_export_main.cpp)
target_link_libraries(ExportContent PRIVATE EngineTools nlohmann_json::nlohmann_json)
shark_configure_target(ExportContent)
shark_classify_target(ExportContent authoring)

add_executable(SharkAuthor src/authoring_main.cpp)
target_link_libraries(SharkAuthor PRIVATE EngineTools nlohmann_json::nlohmann_json)
shark_configure_target(SharkAuthor)
shark_classify_target(SharkAuthor authoring)

if(TARGET EngineClient AND SHARK_NATIVE_GRAPHICS)
    add_executable(SharkAuthorClient src/authoring_main.cpp ../editor/authoring_capture.cpp)
    target_link_libraries(SharkAuthorClient PRIVATE EngineTools EngineClient nlohmann_json::nlohmann_json)
    target_compile_definitions(SharkAuthorClient PRIVATE SHARK_AUTHOR_CLIENT=1)
    shark_configure_target(SharkAuthorClient)
    shark_classify_target(SharkAuthorClient authoring)
endif()
