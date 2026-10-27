add_executable(Tests
    bootstrap_tests.cpp
    core_digest_tests.cpp
    core_error_tests.cpp
    core_identity_tests.cpp
    core_hierarchy_tests.cpp
    core_path_tests.cpp
    core_scene_tests.cpp
    core_scene_edit_tests.cpp
    core_scene_query_tests.cpp
    core_schema_tests.cpp
    core_net_wire_tests.cpp
    core_net_fault_tests.cpp
    core_net_transport_tests.cpp
    core_net_mtls_tests.cpp
    core_net_session_tests.cpp
    core_net_replication_tests.cpp
    core_net_replication_wire_tests.cpp
    net_interest_tests.cpp
    net_adversity_tests.cpp
    core_time_tests.cpp
    core_transform_tests.cpp
    core_work_tests.cpp
    core_world_position_tests.cpp
    server_application_tests.cpp
    server_timing_tests.cpp
    shutdown_signal_tests.cpp
    server_tests.cpp)
target_link_libraries(Tests PRIVATE EngineCore Catch2::Catch2WithMain)
target_include_directories(Tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/core/src")
if(TARGET EngineClient)
    target_sources(Tests PRIVATE client_input_tests.cpp client_window_tests.cpp client_window_launch_tests.cpp client_frame_tests.cpp client_graphics_tests.cpp client_presentation_tests.cpp client_presentation_launch_tests.cpp client_queue_waiter_tests.cpp client_gpu_resources_tests.cpp client_offscreen_tests.cpp client_shader_bundle_tests.cpp)
    target_include_directories(Tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/client/src")
    target_link_libraries(Tests PRIVATE EngineClient)
endif()
target_compile_definitions(Tests PRIVATE
    SHARK_NETWORK_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/network"
    SHARK_EXPECTED_VERSION="${PROJECT_VERSION}"
    SHARK_INJECT_TEST_FAILURE=$<BOOL:${SHARK_INJECT_TEST_FAILURE}>)
shark_configure_target(Tests)
shark_classify_target(Tests test)

if(TARGET EngineStorage)
    add_executable(StorageTests storage_tests.cpp storage_records_tests.cpp)
    target_link_libraries(StorageTests PRIVATE EngineStorage SharkSQLite Catch2::Catch2WithMain)
    target_include_directories(StorageTests PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    shark_configure_target(StorageTests)
    shark_classify_target(StorageTests test)
    add_test(NAME shark.storage.store COMMAND StorageTests "[storage]" --rng-seed 1337)
    set_tests_properties(shark.storage.store PROPERTIES LABELS "cpu;unit;server;integration" TIMEOUT 60)

    add_executable(StorageProcess storage_process.cpp)
    target_link_libraries(StorageProcess PRIVATE EngineStorage SharkSQLite)
    target_include_directories(StorageProcess PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    shark_configure_target(StorageProcess)
    shark_classify_target(StorageProcess test)

    add_executable(StorageCharacterTests storage_character_tests.cpp)
    target_link_libraries(StorageCharacterTests PRIVATE EngineStorage Catch2::Catch2WithMain)
    target_include_directories(StorageCharacterTests PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    target_compile_definitions(StorageCharacterTests PRIVATE
        SHARK_NETWORK_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/network")
    shark_configure_target(StorageCharacterTests)
    shark_classify_target(StorageCharacterTests test)
    add_test(NAME shark.storage.characters COMMAND StorageCharacterTests "[storage-characters]" --rng-seed 1337)
    set_tests_properties(shark.storage.characters PROPERTIES LABELS "cpu;unit;server;integration" TIMEOUT 60)

    add_executable(StorageCharacterProbe storage_character_probe.cpp)
    target_link_libraries(StorageCharacterProbe PRIVATE EngineStorage)
    shark_configure_target(StorageCharacterProbe)
    shark_classify_target(StorageCharacterProbe test)

    add_executable(StorageTransactionTests storage_transaction_tests.cpp)
    target_link_libraries(StorageTransactionTests PRIVATE EngineStorage SharkSQLite Catch2::Catch2WithMain)
    target_include_directories(StorageTransactionTests PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    shark_configure_target(StorageTransactionTests)
    shark_classify_target(StorageTransactionTests test)
    add_test(NAME shark.storage.transactions COMMAND StorageTransactionTests "[storage-transactions]" --rng-seed 1337)
    set_tests_properties(shark.storage.transactions PROPERTIES LABELS "cpu;unit;server;integration" TIMEOUT 90)

    add_executable(StorageRecoveryTests storage_recovery_tests.cpp)
    target_link_libraries(StorageRecoveryTests PRIVATE EngineStorage Catch2::Catch2WithMain)
    target_include_directories(StorageRecoveryTests PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    target_compile_definitions(StorageRecoveryTests PRIVATE
        SHARK_NETWORK_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/network")
    shark_configure_target(StorageRecoveryTests)
    shark_classify_target(StorageRecoveryTests test)
    add_test(NAME shark.storage.recovery COMMAND StorageRecoveryTests "[storage-recovery]" --rng-seed 1337)
    set_tests_properties(shark.storage.recovery PROPERTIES LABELS "cpu;unit;server;integration" TIMEOUT 120)

    add_executable(StorageRecoveryProbe storage_recovery_probe.cpp storage_recovery_vfs.cpp)
    target_link_libraries(StorageRecoveryProbe PRIVATE EngineStorage SharkSQLite)
    target_include_directories(StorageRecoveryProbe PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    shark_configure_target(StorageRecoveryProbe)
    shark_classify_target(StorageRecoveryProbe test)

    add_executable(StorageMigrationTests storage_migration_tests.cpp)
    target_link_libraries(StorageMigrationTests PRIVATE EngineStorage SharkSQLite Catch2::Catch2WithMain)
    target_include_directories(StorageMigrationTests PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    target_compile_definitions(StorageMigrationTests PRIVATE
        SHARK_STORAGE_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/storage"
        SHARK_NETWORK_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/network")
    shark_configure_target(StorageMigrationTests)
    shark_classify_target(StorageMigrationTests test)
    add_test(NAME shark.storage.migration COMMAND StorageMigrationTests "[storage-migration]" --rng-seed 1337)
    set_tests_properties(shark.storage.migration PROPERTIES LABELS "cpu;unit;server;integration" TIMEOUT 120)

    add_executable(StorageMigrationProbe storage_migration_probe.cpp storage_recovery_vfs.cpp)
    target_link_libraries(StorageMigrationProbe PRIVATE EngineStorage SharkSQLite)
    target_include_directories(StorageMigrationProbe PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    target_compile_definitions(StorageMigrationProbe PRIVATE
        SHARK_STORAGE_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/storage")
    shark_configure_target(StorageMigrationProbe)
    shark_classify_target(StorageMigrationProbe test)

    add_executable(StorageIntegrationProbe storage_integration_probe.cpp storage_recovery_vfs.cpp)
    target_link_libraries(StorageIntegrationProbe PRIVATE EngineStorage SharkSQLite)
    target_include_directories(StorageIntegrationProbe PRIVATE "${PROJECT_SOURCE_DIR}/server/storage/src")
    shark_configure_target(StorageIntegrationProbe)
    shark_classify_target(StorageIntegrationProbe test)

    add_executable(StorageTransactionProbe storage_transaction_probe.cpp)
    target_link_libraries(StorageTransactionProbe PRIVATE EngineStorage)
    shark_configure_target(StorageTransactionProbe)
    shark_classify_target(StorageTransactionProbe test)
endif()

if(TARGET EngineTools)
    add_executable(AssetImportJobsTests asset_import_jobs_tests.cpp)
    target_link_libraries(AssetImportJobsTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(AssetImportJobsTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(AssetImportJobsTests)
    shark_classify_target(AssetImportJobsTests test)
    add_test(NAME shark.tools.asset-import-jobs COMMAND AssetImportJobsTests "[asset-import-jobs]" --rng-seed 1337
        --reporter xml --out "${CMAKE_CURRENT_BINARY_DIR}/asset-import-jobs-cases.xml")
    set_tests_properties(shark.tools.asset-import-jobs PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 90)

    add_executable(AssetImportTests asset_import_tests.cpp)
    target_link_libraries(AssetImportTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(AssetImportTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(AssetImportTests)
    shark_classify_target(AssetImportTests test)
    add_test(NAME shark.tools.asset-import COMMAND AssetImportTests "[asset-import]" --rng-seed 1337
        --reporter xml --out "${CMAKE_CURRENT_BINARY_DIR}/asset-import-cases.xml")
    set_tests_properties(shark.tools.asset-import PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 90)
    add_executable(AssetImportProcess asset_import_process.cpp)
    target_link_libraries(AssetImportProcess PRIVATE EngineTools)
    target_include_directories(AssetImportProcess PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(AssetImportProcess)
    shark_classify_target(AssetImportProcess test)

    add_executable(AssetCatalogTests asset_catalog_tests.cpp)
    target_link_libraries(AssetCatalogTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(AssetCatalogTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(AssetCatalogTests)
    shark_classify_target(AssetCatalogTests test)
    add_test(NAME shark.tools.asset-catalog COMMAND AssetCatalogTests "[assets]" --rng-seed 1337
        --reporter xml --out "${CMAKE_CURRENT_BINARY_DIR}/asset-catalog-cases.xml")
    set_tests_properties(shark.tools.asset-catalog PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)

    add_executable(FeatureWorldTests feature_world_tests.cpp)
    target_link_libraries(FeatureWorldTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_compile_definitions(FeatureWorldTests PRIVATE
        SHARK_FEATUREWORLD_DIRECTORY="${PROJECT_SOURCE_DIR}/assets/FeatureWorld"
        SHARK_FEATUREWORLD_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}")
    shark_configure_target(FeatureWorldTests)
    shark_classify_target(FeatureWorldTests test)
    add_test(NAME shark.tools.featureworld COMMAND FeatureWorldTests "[featureworld]" --rng-seed 1337
        --reporter xml --out "${CMAKE_CURRENT_BINARY_DIR}/featureworld-cases.xml")
    set_tests_properties(shark.tools.featureworld PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)

    add_executable(SceneRunnerTests scene_runner_tests.cpp)
    target_link_libraries(SceneRunnerTests PRIVATE EngineTools Catch2::Catch2WithMain)
    shark_configure_target(SceneRunnerTests)
    shark_classify_target(SceneRunnerTests test)
    add_test(NAME shark.tools.scene-runner COMMAND SceneRunnerTests "[scene-runner]" --rng-seed 1337
        --reporter xml --out "${CMAKE_CURRENT_BINARY_DIR}/scene-runner-cases.xml")
    set_tests_properties(shark.tools.scene-runner PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)

    add_executable(SourceDocumentTests source_document_tests.cpp)
    target_link_libraries(SourceDocumentTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_compile_definitions(SourceDocumentTests PRIVATE
        SHARK_SOURCE_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/source_documents")
    shark_configure_target(SourceDocumentTests)
    shark_classify_target(SourceDocumentTests test)
    add_test(NAME shark.tools.source-documents COMMAND SourceDocumentTests "[source-documents]" --rng-seed 1337)
    set_tests_properties(shark.tools.source-documents PROPERTIES LABELS "cpu;unit;tooling" TIMEOUT 60)

    add_executable(AuthoredSceneTests authored_scene_tests.cpp)
    target_link_libraries(AuthoredSceneTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(AuthoredSceneTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(AuthoredSceneTests)
    shark_classify_target(AuthoredSceneTests test)
    add_test(NAME shark.tools.authored-scene COMMAND AuthoredSceneTests "[authored-scene]" --rng-seed 1337)
    set_tests_properties(shark.tools.authored-scene PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)

    add_executable(AuthoringCommandTests authoring_command_tests.cpp authoring_allocation_fault.cpp)
    target_link_libraries(AuthoringCommandTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(AuthoringCommandTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(AuthoringCommandTests)
    shark_classify_target(AuthoringCommandTests test)
    add_test(NAME shark.tools.authoring-retries COMMAND AuthoringCommandTests "[authoring-retries]" --rng-seed 1337)
    set_tests_properties(shark.tools.authoring-retries PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)
    add_test(NAME shark.tools.authoring-history COMMAND AuthoringCommandTests "[authoring-history]" --rng-seed 1337)
    set_tests_properties(shark.tools.authoring-history PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)
    add_test(NAME shark.tools.authoring-batches COMMAND AuthoringCommandTests "[authoring-batches]" --rng-seed 1337)
    set_tests_properties(shark.tools.authoring-batches PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)
    add_test(NAME shark.tools.authoring-commands COMMAND AuthoringCommandTests "[authoring-commands]" --rng-seed 1337)
    set_tests_properties(shark.tools.authoring-commands PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 60)

    add_executable(ProjectFilesTests project_files_tests.cpp)
    target_link_libraries(ProjectFilesTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(ProjectFilesTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(ProjectFilesTests)
    shark_classify_target(ProjectFilesTests test)
    add_test(NAME shark.tools.project-files COMMAND ProjectFilesTests "[project-files]" --rng-seed 1337)
    set_tests_properties(shark.tools.project-files PROPERTIES LABELS "cpu;unit;tooling;integration" TIMEOUT 90)

    add_executable(PersistenceIntegrationTests persistence_integration_tests.cpp)
    target_link_libraries(PersistenceIntegrationTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(PersistenceIntegrationTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    target_compile_definitions(PersistenceIntegrationTests PRIVATE
        SHARK_SOURCE_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/source_documents")
    shark_configure_target(PersistenceIntegrationTests)
    shark_classify_target(PersistenceIntegrationTests test)
    add_test(NAME shark.tools.persistence-integration COMMAND PersistenceIntegrationTests "[persistence-integration]" --rng-seed 1337)
    set_tests_properties(shark.tools.persistence-integration PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 90)

    add_executable(AuthoringIntegrationTests authoring_integration_tests.cpp)
    target_link_libraries(AuthoringIntegrationTests PRIVATE EngineTools Catch2::Catch2WithMain)
    target_include_directories(AuthoringIntegrationTests PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    target_compile_definitions(AuthoringIntegrationTests PRIVATE
        SHARK_SOURCE_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/fixtures/source_documents")
    shark_configure_target(AuthoringIntegrationTests)
    shark_classify_target(AuthoringIntegrationTests test)
    add_test(NAME shark.tools.authoring-integration COMMAND AuthoringIntegrationTests "[authoring-integration]" --rng-seed 1337)
    set_tests_properties(shark.tools.authoring-integration PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 90)

    add_executable(ProjectFilesProcess project_files_process.cpp)
    target_link_libraries(ProjectFilesProcess PRIVATE EngineTools)
    target_include_directories(ProjectFilesProcess PRIVATE "${PROJECT_SOURCE_DIR}/engine_tools/src")
    shark_configure_target(ProjectFilesProcess)
    shark_classify_target(ProjectFilesProcess test)

    add_executable(AuthoringRetryProbe authoring_retry_probe.cpp)
    target_link_libraries(AuthoringRetryProbe PRIVATE EngineTools)
    shark_configure_target(AuthoringRetryProbe)
    shark_classify_target(AuthoringRetryProbe test)
endif()

# Explicit registration avoids executing a sanitizer binary during the build.
add_test(NAME shark.unit COMMAND Tests "[bootstrap]" --rng-seed 1337)
set_tests_properties(shark.unit PROPERTIES LABELS "cpu;unit" TIMEOUT 10)
add_test(NAME shark.failure-sentinel COMMAND Tests "[failure-sentinel]" --rng-seed 1337)
set_tests_properties(shark.failure-sentinel PROPERTIES LABELS "cpu;unit" TIMEOUT 10)
add_test(NAME shark.server.unit COMMAND Tests "[server]" --rng-seed 1337)
set_tests_properties(shark.server.unit PROPERTIES LABELS "cpu;server;unit" TIMEOUT 10)
add_test(NAME shark.core.contracts COMMAND Tests "[core]" --rng-seed 1337)
add_test(NAME shark.core.content-digest COMMAND Tests "[content-digest]" --rng-seed 1337)
set_tests_properties(shark.core.content-digest PROPERTIES LABELS "cpu;unit;server" TIMEOUT 15)
set_tests_properties(shark.core.contracts PROPERTIES LABELS "cpu;unit" TIMEOUT 15)
add_test(NAME shark.core.lifecycle COMMAND Tests "[lifecycle]" --rng-seed 1337)
set_tests_properties(shark.core.lifecycle PROPERTIES LABELS "cpu;unit;server" TIMEOUT 15)
add_test(NAME shark.core.timing COMMAND Tests "[timing]" --rng-seed 1337)
set_tests_properties(shark.core.timing PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.work COMMAND Tests "[work]" --rng-seed 1337)
set_tests_properties(shark.core.work PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.scene-edit COMMAND Tests "[scene-edit]" --rng-seed 1337)
set_tests_properties(shark.core.scene-edit PROPERTIES LABELS "cpu;unit;server" TIMEOUT 30)
add_test(NAME shark.core.scene COMMAND Tests "[scene]" --rng-seed 1337)
set_tests_properties(shark.core.scene PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.world-position COMMAND Tests "[world-position]" --rng-seed 1337)
set_tests_properties(shark.core.world-position PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.hierarchy COMMAND Tests "[hierarchy]" --rng-seed 1337)
set_tests_properties(shark.core.hierarchy PROPERTIES LABELS "cpu;unit;server" TIMEOUT 30)
add_test(NAME shark.core.transform COMMAND Tests "[transform]" --rng-seed 1337)
set_tests_properties(shark.core.transform PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.schema COMMAND Tests "[schema]" --rng-seed 1337)
set_tests_properties(shark.core.schema PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.scene-query COMMAND Tests "[scene-query]" --rng-seed 1337)
set_tests_properties(shark.core.scene-query PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.net-wire COMMAND Tests "[net-wire]" --rng-seed 1337)
set_tests_properties(shark.core.net-wire PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.net-fault COMMAND Tests "[net-fault]" --rng-seed 1337)
set_tests_properties(shark.core.net-fault PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.net-transport COMMAND Tests "[net-transport]" --rng-seed 1337)
set_tests_properties(shark.core.net-transport PROPERTIES LABELS "cpu;unit;server" TIMEOUT 30)
add_test(NAME shark.core.net-mtls COMMAND Tests "[net-mtls]" --rng-seed 1337)
set_tests_properties(shark.core.net-mtls PROPERTIES LABELS "cpu;unit;server" TIMEOUT 30)
add_test(NAME shark.core.net-session COMMAND Tests "[net-session]" --rng-seed 1337)
set_tests_properties(shark.core.net-session PROPERTIES LABELS "cpu;unit;server" TIMEOUT 30)
add_test(NAME shark.core.net-replication-wire COMMAND Tests "[net-replication-wire]" --rng-seed 1337)
set_tests_properties(shark.core.net-replication-wire PROPERTIES LABELS "cpu;unit;server" TIMEOUT 20)
add_test(NAME shark.core.net-replication COMMAND Tests "[net-replication]" --rng-seed 1337)
set_tests_properties(shark.core.net-replication PROPERTIES LABELS "cpu;unit;server" TIMEOUT 30)
add_test(NAME shark.core.net-interest COMMAND Tests "[net-interest]" --rng-seed 1337)
set_tests_properties(shark.core.net-interest PROPERTIES LABELS "cpu;unit;server" TIMEOUT 30)
add_test(NAME shark.core.net-adversity COMMAND Tests "[net-adversity]" --rng-seed 1337)
set_tests_properties(shark.core.net-adversity PROPERTIES LABELS "cpu;unit;server" TIMEOUT 60)
if(TARGET EngineClient)
    add_executable(PresentationFaults presentation_faults.cpp)
    target_include_directories(PresentationFaults PRIVATE "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/engine/client/src")
    target_link_libraries(PresentationFaults PRIVATE EngineClient)
    target_compile_definitions(PresentationFaults PRIVATE
        SHARK_FAULT_NATIVE_GRAPHICS=$<BOOL:${SHARK_NATIVE_GRAPHICS}>)
    shark_configure_target(PresentationFaults)
    shark_classify_target(PresentationFaults test)
    add_test(NAME shark.client.input COMMAND Tests "[input]" --rng-seed 1337)
    set_tests_properties(shark.client.input PROPERTIES LABELS "cpu;unit;client" TIMEOUT 15)
    add_test(NAME shark.client.window COMMAND Tests "[window]" --rng-seed 1337)
    set_tests_properties(shark.client.window PROPERTIES LABELS "cpu;unit;client" TIMEOUT 20)
    add_test(NAME shark.client.frame COMMAND Tests "[frame]" --rng-seed 1337)
    set_tests_properties(shark.client.frame PROPERTIES LABELS "cpu;unit;client" TIMEOUT 20)
    add_test(NAME shark.client.graphics COMMAND Tests "[graphics-device]" --rng-seed 1337)
    set_tests_properties(shark.client.graphics PROPERTIES LABELS "cpu;unit;client" TIMEOUT 20)
    add_test(NAME shark.client.presentation COMMAND Tests "[presentation]" --rng-seed 1337)
    set_tests_properties(shark.client.presentation PROPERTIES LABELS "cpu;unit;client" TIMEOUT 20)
    add_test(NAME shark.client.gpu-resources COMMAND Tests "[gpu-resources]" --rng-seed 1337)
    set_tests_properties(shark.client.gpu-resources PROPERTIES LABELS "cpu;unit;client" TIMEOUT 20)
    add_test(NAME shark.client.offscreen COMMAND Tests "[offscreen]" --rng-seed 1337)
    set_tests_properties(shark.client.offscreen PROPERTIES LABELS "cpu;unit;client" TIMEOUT 20)
    add_test(NAME shark.client.shader-bundle COMMAND Tests "[shader-bundle]" --rng-seed 1337)
    set_tests_properties(shark.client.shader-bundle PROPERTIES LABELS "cpu;unit;client" TIMEOUT 20)
    if(SHARK_NATIVE_WINDOW)
        # Explicit developer smoke, never launched by ordinary display-free CTest.
        add_executable(WindowSmoke window_smoke.cpp)
        target_include_directories(WindowSmoke PRIVATE "${PROJECT_SOURCE_DIR}/engine/client/src")
        target_link_libraries(WindowSmoke PRIVATE EngineClient)
        shark_configure_target(WindowSmoke)
        shark_classify_target(WindowSmoke test)
    endif()
    if(SHARK_NATIVE_GRAPHICS)
        # Explicit native device qualification; ordinary CTest stays GPU-free.
        add_executable(GraphicsDeviceSmoke graphics_device_smoke.cpp)
        target_include_directories(GraphicsDeviceSmoke PRIVATE "${PROJECT_SOURCE_DIR}")
        target_link_libraries(GraphicsDeviceSmoke PRIVATE EngineClient)
        shark_configure_target(GraphicsDeviceSmoke)
        shark_classify_target(GraphicsDeviceSmoke test)
        foreach(shark_offscreen_fixture IN ITEMS OffscreenSmoke OffscreenFaults CubeSmoke CubeFaults)
            if(shark_offscreen_fixture STREQUAL "OffscreenSmoke")
                set(shark_offscreen_source offscreen_smoke.cpp)
            elseif(shark_offscreen_fixture STREQUAL "CubeSmoke")
                set(shark_offscreen_source cube_smoke.cpp)
            elseif(shark_offscreen_fixture STREQUAL "CubeFaults")
                set(shark_offscreen_source cube_faults.cpp)
            else()
                set(shark_offscreen_source offscreen_faults.cpp)
            endif()
            add_executable(${shark_offscreen_fixture} ${shark_offscreen_source})
            target_include_directories(${shark_offscreen_fixture} PRIVATE "${PROJECT_SOURCE_DIR}"
                "${PROJECT_SOURCE_DIR}/engine/client/src")
            target_link_libraries(${shark_offscreen_fixture} PRIVATE EngineClient)
            shark_configure_target(${shark_offscreen_fixture})
            shark_classify_target(${shark_offscreen_fixture} test)
        endforeach()
        add_executable(PresentationSmoke presentation_smoke.cpp)
        target_include_directories(PresentationSmoke PRIVATE "${PROJECT_SOURCE_DIR}")
        target_link_libraries(PresentationSmoke PRIVATE EngineClient)
        shark_configure_target(PresentationSmoke)
        shark_classify_target(PresentationSmoke test)
        add_executable(PresentationStress presentation_stress.cpp)
        target_include_directories(PresentationStress PRIVATE "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/engine/client/src")
        target_link_libraries(PresentationStress PRIVATE EngineClient Psapi)
        shark_configure_target(PresentationStress)
        shark_classify_target(PresentationStress test)
        if(SHARK_SANITIZER STREQUAL "none")
            string(TOLOWER "${CMAKE_BUILD_TYPE}" shark_presentation_profile)
        else()
            set(shark_presentation_profile "asan")
        endif()
        target_compile_definitions(PresentationStress PRIVATE
            SHARK_GRAPHICS_PROFILE="${shark_presentation_profile}")

        file(SHA256 "${PROJECT_SOURCE_DIR}/dependencies.lock.json" shark_graphics_lock_sha256)
        file(GLOB_RECURSE shark_graphics_source_paths CONFIGURE_DEPENDS
            RELATIVE "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/engine/core/*"
            "${PROJECT_SOURCE_DIR}/engine/client/*")
        list(FILTER shark_graphics_source_paths INCLUDE REGEX "(\\.(cpp|hpp|manifest)|/CMakeLists\\.txt)$")
        file(GLOB shark_graphics_cmake_paths CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/cmake/*.cmake")
        list(APPEND shark_graphics_source_paths ${shark_graphics_cmake_paths}
            "CMakeLists.txt" "dependencies.lock.json" "engine/CMakeLists.txt"
            "tests/CMakeLists.txt" "tests/graphics_device_smoke.cpp" "probes/graphics/report.hpp")
        list(REMOVE_DUPLICATES shark_graphics_source_paths)
        list(SORT shark_graphics_source_paths)
        set(shark_graphics_source_material "")
        foreach(shark_graphics_source IN LISTS shark_graphics_source_paths)
            file(SHA256 "${PROJECT_SOURCE_DIR}/${shark_graphics_source}" shark_graphics_file_sha256)
            string(APPEND shark_graphics_source_material "${shark_graphics_source}\n${shark_graphics_file_sha256}\n")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${PROJECT_SOURCE_DIR}/${shark_graphics_source}")
        endforeach()
        string(SHA256 shark_graphics_source_sha256 "${shark_graphics_source_material}")
        target_compile_definitions(GraphicsDeviceSmoke PRIVATE
            SHARK_GRAPHICS_LOCK_SHA256="${shark_graphics_lock_sha256}"
            SHARK_GRAPHICS_SOURCE_SHA256="${shark_graphics_source_sha256}"
            SHARK_GRAPHICS_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
        # Both fixtures hash their actual compiled inputs independently. Keep
        # this substitution in step with presentation_smoke.source_inputs().
        list(REMOVE_ITEM shark_graphics_source_paths "tests/graphics_device_smoke.cpp")
        list(APPEND shark_graphics_source_paths "tests/presentation_smoke.cpp")
        list(SORT shark_graphics_source_paths)
        set(shark_graphics_source_material "")
        foreach(shark_graphics_source IN LISTS shark_graphics_source_paths)
            file(SHA256 "${PROJECT_SOURCE_DIR}/${shark_graphics_source}" shark_graphics_file_sha256)
            string(APPEND shark_graphics_source_material "${shark_graphics_source}\n${shark_graphics_file_sha256}\n")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${PROJECT_SOURCE_DIR}/${shark_graphics_source}")
        endforeach()
        string(SHA256 shark_graphics_source_sha256 "${shark_graphics_source_material}")
        target_compile_definitions(PresentationSmoke PRIVATE
            SHARK_GRAPHICS_LOCK_SHA256="${shark_graphics_lock_sha256}"
            SHARK_GRAPHICS_SOURCE_SHA256="${shark_graphics_source_sha256}"
            SHARK_GRAPHICS_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
        list(REMOVE_ITEM shark_graphics_source_paths "tests/presentation_smoke.cpp")
        foreach(shark_fixture IN ITEMS PresentationStress PresentationFaults OffscreenSmoke OffscreenFaults CubeSmoke CubeFaults)
            if(shark_fixture STREQUAL "PresentationStress")
                set(shark_fixture_source "tests/presentation_stress.cpp")
            elseif(shark_fixture STREQUAL "PresentationFaults")
                set(shark_fixture_source "tests/presentation_faults.cpp")
            elseif(shark_fixture STREQUAL "OffscreenSmoke")
                set(shark_fixture_source "tests/offscreen_smoke.cpp")
            elseif(shark_fixture STREQUAL "CubeSmoke")
                set(shark_fixture_source "tests/cube_smoke.cpp")
            elseif(shark_fixture STREQUAL "CubeFaults")
                set(shark_fixture_source "tests/cube_faults.cpp")
            else()
                set(shark_fixture_source "tests/offscreen_faults.cpp")
            endif()
            set(shark_fixture_sources ${shark_graphics_source_paths} "${shark_fixture_source}")
            list(SORT shark_fixture_sources)
            set(shark_fixture_material "")
            foreach(shark_source IN LISTS shark_fixture_sources)
                file(SHA256 "${PROJECT_SOURCE_DIR}/${shark_source}" shark_file_sha256)
                string(APPEND shark_fixture_material "${shark_source}\n${shark_file_sha256}\n")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${PROJECT_SOURCE_DIR}/${shark_source}")
            endforeach()
            string(SHA256 shark_fixture_sha256 "${shark_fixture_material}")
            target_compile_definitions(${shark_fixture} PRIVATE
                SHARK_GRAPHICS_LOCK_SHA256="${shark_graphics_lock_sha256}"
                SHARK_GRAPHICS_SOURCE_SHA256="${shark_fixture_sha256}"
                SHARK_GRAPHICS_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
        endforeach()
    endif()
endif()

foreach(shark_application IN ITEMS Runtime Editor)
    if(TARGET "${shark_application}")
        string(TOLOWER "${shark_application}" shark_application_lower)
        add_test(NAME "shark.${shark_application_lower}.cli"
            COMMAND "${CMAKE_COMMAND}"
                "-DSHARK_EXECUTABLE=$<TARGET_FILE:${shark_application}>"
                "-DSHARK_APPLICATION=${shark_application}"
                "-DSHARK_VERSION=${PROJECT_VERSION}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCli.cmake")
        set_tests_properties("shark.${shark_application_lower}.cli" PROPERTIES
            LABELS "cpu;integration" TIMEOUT 30)
        add_test(NAME "shark.${shark_application_lower}.window-cli"
            COMMAND "${CMAKE_COMMAND}"
                "-DSHARK_EXECUTABLE=$<TARGET_FILE:${shark_application}>"
                "-DSHARK_NATIVE_WINDOW=${SHARK_NATIVE_WINDOW}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckWindowCli.cmake")
        set_tests_properties("shark.${shark_application_lower}.window-cli" PROPERTIES
            LABELS "cpu;integration;client" TIMEOUT 30)
        add_test(NAME "shark.${shark_application_lower}.presentation-cli"
            COMMAND "${CMAKE_COMMAND}"
                "-DSHARK_EXECUTABLE=$<TARGET_FILE:${shark_application}>"
                "-DSHARK_NATIVE_GRAPHICS=${SHARK_NATIVE_GRAPHICS}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckPresentationLaunchCli.cmake")
        set_tests_properties("shark.${shark_application_lower}.presentation-cli" PROPERTIES
            LABELS "cpu;integration;client" TIMEOUT 30)
    endif()
endforeach()
add_test(NAME shark.boundary.fixture
    COMMAND "${CMAKE_COMMAND}"
        "-DSHARK_SOURCE=${PROJECT_SOURCE_DIR}"
        "-DSHARK_FIXTURE_BINARY=${CMAKE_CURRENT_BINARY_DIR}/boundary-fixture"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckBoundaryFixture.cmake")
set_tests_properties(shark.boundary.fixture PROPERTIES LABELS "cpu;boundary" TIMEOUT 30)

# Repository checks are supplied by the foundation tooling without making them
# dependencies of the runtime-only configuration.
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_REGISTRY NEVER)
set(Python3_FIND_FRAMEWORK NEVER)
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
if(TARGET SceneRunner)
    add_test(NAME shark.tools.scene-qualification
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_scene_qualification_process.py"
            --runner "$<TARGET_FILE:SceneRunner>" --build-dir "${PROJECT_BINARY_DIR}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/scene-qualification.json")
    set_tests_properties(shark.tools.scene-qualification PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 90)

    add_test(NAME shark.tools.scene-execution
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_scene_execution_process.py"
            --runner "$<TARGET_FILE:SceneRunner>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/scene-execution-process.json")
    set_tests_properties(shark.tools.scene-execution PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 90)

    add_test(NAME shark.tooling.scene-execution-evidence
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_scene_execution.py" -v)
    set_tests_properties(shark.tooling.scene-execution-evidence PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 90)

    add_test(NAME shark.tools.featureworld-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_feature_world_process.py"
            --runner "$<TARGET_FILE:SceneRunner>"
            --fixture "${PROJECT_SOURCE_DIR}/assets/FeatureWorld"
            --manifest "${PROJECT_SOURCE_DIR}/docs/coverage/requirements.json"
            --output "${CMAKE_CURRENT_BINARY_DIR}/featureworld-process.json")
    set_tests_properties(shark.tools.featureworld-process PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 60)

    add_test(NAME shark.tools.scene-runner-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_scene_runner_process.py"
            --runner "$<TARGET_FILE:SceneRunner>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/scene-runner-process.json")
    set_tests_properties(shark.tools.scene-runner-process PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 60)
endif()
if(TARGET EngineStorage)
    add_test(NAME shark.storage.process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_storage_process.py"
            --probe "$<TARGET_FILE:StorageProcess>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/storage-process.json")
    set_tests_properties(shark.storage.process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 90)
    add_test(NAME shark.storage.characters-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_storage_characters_process.py"
            --probe "$<TARGET_FILE:StorageCharacterProbe>"
            --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
            --output "${CMAKE_CURRENT_BINARY_DIR}/storage-characters-process.json")
    set_tests_properties(shark.storage.characters-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 180)
    add_test(NAME shark.storage.transactions-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_storage_transactions_process.py"
            --probe "$<TARGET_FILE:StorageTransactionProbe>"
            --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
            --output "${CMAKE_CURRENT_BINARY_DIR}/storage-transactions-process.json")
    set_tests_properties(shark.storage.transactions-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 180)
    add_test(NAME shark.storage.recovery-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_storage_recovery_process.py"
            --probe "$<TARGET_FILE:StorageRecoveryProbe>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/storage-recovery-process.json")
    set_tests_properties(shark.storage.recovery-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 240)

    add_test(NAME shark.storage.migration-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_storage_migration_process.py"
            --probe "$<TARGET_FILE:StorageMigrationProbe>"
            --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/storage"
            --output "${CMAKE_CURRENT_BINARY_DIR}/storage-migration-process.json")
    set_tests_properties(shark.storage.migration-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 240)

    add_test(NAME shark.storage.integration-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_storage_integration_process.py"
            --probe "$<TARGET_FILE:StorageIntegrationProbe>"
            --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
            --storage-fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/storage"
            --output "${CMAKE_CURRENT_BINARY_DIR}/storage-integration-process.json")
    set_tests_properties(shark.storage.integration-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 300)
endif()
if(TARGET ProjectFilesProcess)
    add_test(NAME shark.tools.asset-import-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_asset_import_process.py"
            --probe "$<TARGET_FILE:AssetImportProcess>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/asset-import-process.json")
    set_tests_properties(shark.tools.asset-import-process PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 120)
    add_test(NAME shark.tools.asset-jobs-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_asset_jobs_process.py"
            --probe "$<TARGET_FILE:AssetImportProcess>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/asset-jobs-process.json")
    set_tests_properties(shark.tools.asset-jobs-process PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 150)
    add_test(NAME shark.tools.project-files-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_project_files_process.py"
            --probe "$<TARGET_FILE:ProjectFilesProcess>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/project-files-process.json")
    set_tests_properties(shark.tools.project-files-process PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 90)
endif()

if(TARGET AuthoringRetryProbe)
    add_test(NAME shark.tools.authoring-retries-process
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_authoring_retries_process.py"
            --probe "$<TARGET_FILE:AuthoringRetryProbe>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/authoring-retries-process.json" --rng-seed 1337)
    set_tests_properties(shark.tools.authoring-retries-process PROPERTIES LABELS "cpu;tooling;integration" TIMEOUT 90)
endif()
add_executable(NetworkProbe network_probe.cpp)
target_link_libraries(NetworkProbe PRIVATE EngineCore)
shark_configure_target(NetworkProbe)
shark_classify_target(NetworkProbe test)
add_test(NAME shark.core.net-process
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_network_transport.py"
        --probe "$<TARGET_FILE:NetworkProbe>"
        --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
        --output "${CMAKE_CURRENT_BINARY_DIR}/network-process.json")
set_tests_properties(shark.core.net-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 60)
add_executable(SessionProbe session_probe.cpp)
target_link_libraries(SessionProbe PRIVATE EngineCore)
shark_configure_target(SessionProbe)
shark_classify_target(SessionProbe test)
add_test(NAME shark.core.net-sessions-process
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_network_sessions.py"
        --probe "$<TARGET_FILE:SessionProbe>"
        --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
        --output "${CMAKE_CURRENT_BINARY_DIR}/network-sessions.json")
set_tests_properties(shark.core.net-sessions-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 90)
add_executable(ReplicationProbe replication_probe.cpp)
target_link_libraries(ReplicationProbe PRIVATE EngineCore)
shark_configure_target(ReplicationProbe)
shark_classify_target(ReplicationProbe test)
add_test(NAME shark.core.net-replication-process
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_network_replication.py"
        --probe "$<TARGET_FILE:ReplicationProbe>"
        --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
        --output "${CMAKE_CURRENT_BINARY_DIR}/replication-process.json")
set_tests_properties(shark.core.net-replication-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 90)
add_executable(NetInterestProbe net_interest_probe.cpp)
target_link_libraries(NetInterestProbe PRIVATE EngineCore)
shark_configure_target(NetInterestProbe)
shark_classify_target(NetInterestProbe test)
add_test(NAME shark.core.net-interest-process
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_net_interest.py"
        --probe "$<TARGET_FILE:NetInterestProbe>"
        --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
        --output "${CMAKE_CURRENT_BINARY_DIR}/interest-process.json")
set_tests_properties(shark.core.net-interest-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 120)
add_executable(NetAdversityProbe net_adversity_probe.cpp)
target_link_libraries(NetAdversityProbe PRIVATE EngineCore)
shark_configure_target(NetAdversityProbe)
shark_classify_target(NetAdversityProbe test)
add_test(NAME shark.core.net-adversity-process
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_net_adversity.py"
        --probe "$<TARGET_FILE:NetAdversityProbe>"
        --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/network"
        --output "${CMAKE_CURRENT_BINARY_DIR}/adversity-process.json")
set_tests_properties(shark.core.net-adversity-process PROPERTIES LABELS "cpu;integration;server" TIMEOUT 360)
foreach(shark_tooling_fixture IN ITEMS presentation-stress presentation-faults runtime-package shader-pipeline offscreen-smoke offscreen-faults cube-smoke cube-faults)
    string(REPLACE "-" "_" shark_tooling_source "${shark_tooling_fixture}")
    add_test(NAME "shark.tooling.${shark_tooling_fixture}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_${shark_tooling_source}.py" -v)
    set_tests_properties("shark.tooling.${shark_tooling_fixture}" PROPERTIES
        LABELS "cpu;tooling" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endforeach()
set_tests_properties(shark.tooling.runtime-package PROPERTIES LABELS "cpu;tooling;boundary")
if(TARGET OffscreenSmoke)
    add_test(NAME shark.client.offscreen-cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_offscreen_cli.py"
            --executable "$<TARGET_FILE:OffscreenSmoke>")
    set_tests_properties(shark.client.offscreen-cli PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
if(TARGET CubeSmoke)
    add_test(NAME shark.client.cube-cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_cube_cli.py"
            --executable "$<TARGET_FILE:CubeSmoke>")
    set_tests_properties(shark.client.cube-cli PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
if(TARGET CubeFaults)
    add_test(NAME shark.client.cube-faults-cli
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/cube_faults.py"
            --executable "$<TARGET_FILE:CubeFaults>" --cli-only)
    set_tests_properties(shark.client.cube-faults-cli PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
if(TARGET OffscreenFaults)
    add_test(NAME shark.client.offscreen-faults-cli
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/offscreen_faults.py"
            --executable "$<TARGET_FILE:OffscreenFaults>" --cli-only)
    set_tests_properties(shark.client.offscreen-faults-cli PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
if(TARGET PresentationFaults)
    add_test(NAME shark.client.presentation-faults
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/presentation_faults.py"
            --executable "$<TARGET_FILE:PresentationFaults>" --mode cpu)
    set_tests_properties(shark.client.presentation-faults PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 45 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
if(TARGET PresentationStress)
    add_test(NAME shark.client.presentation-stress-cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_presentation_stress_cli.py"
            --executable "$<TARGET_FILE:PresentationStress>")
    set_tests_properties(shark.client.presentation-stress-cli PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
add_test(NAME shark.tooling.graphics-device-evidence
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_device_smoke.py" -v)
set_tests_properties(shark.tooling.graphics-device-evidence PROPERTIES
    LABELS "cpu;tooling" TIMEOUT 15 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.tooling.presentation-evidence
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_presentation_smoke.py" -v)
set_tests_properties(shark.tooling.presentation-evidence PROPERTIES
    LABELS "cpu;tooling" TIMEOUT 15 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
if(TARGET GraphicsDeviceSmoke)
    add_test(NAME shark.client.graphics-cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_graphics_device_cli.py"
            --executable "$<TARGET_FILE:GraphicsDeviceSmoke>")
    set_tests_properties(shark.client.graphics-cli PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
if(TARGET PresentationSmoke)
    add_test(NAME shark.client.presentation-cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_presentation_cli.py"
            --executable "$<TARGET_FILE:PresentationSmoke>")
    set_tests_properties(shark.client.presentation-cli PROPERTIES
        LABELS "cpu;integration;client" TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
if(TARGET Server)
    add_test(NAME shark.server.lifecycle
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_server.py"
            --executable "$<TARGET_FILE:Server>" --expected-version "${PROJECT_VERSION}")
    set_tests_properties(shark.server.lifecycle PROPERTIES LABELS "cpu;server;integration" TIMEOUT 60)
endif()
add_test(NAME shark.tooling.processes
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_process_runner.py" -v)
set_tests_properties(shark.tooling.processes PROPERTIES LABELS "cpu;tooling" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.tooling.sources
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_clean_source.py" -v)
set_tests_properties(shark.tooling.sources PROPERTIES LABELS "cpu;tooling" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.contract
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/validate_contract.py" --self-test)
set_tests_properties(shark.contract PROPERTIES LABELS "cpu;contract" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
if(EXISTS "${PROJECT_SOURCE_DIR}/tools/check_repository.py")
    add_test(NAME shark.repository
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_repository.py")
    set_tests_properties(shark.repository PROPERTIES LABELS "cpu;contract" TIMEOUT 30
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

add_test(NAME shark.tooling.server-closure
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_server_closure.py" -v)
set_tests_properties(shark.tooling.server-closure PROPERTIES
    LABELS "cpu;tooling;boundary;server" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.boundary.headers
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/check_server_header_fixture.py"
        --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}" --compiler "${CMAKE_CXX_COMPILER}"
        --source-dir "${PROJECT_SOURCE_DIR}/tests/boundary_fixture/header_leak"
        --build-dir "${CMAKE_CURRENT_BINARY_DIR}/header-fixture")
set_tests_properties(shark.boundary.headers PROPERTIES LABELS "cpu;tooling;boundary;server" TIMEOUT 60
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.tooling.headless-environment
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_headless_environment.py" -v)
set_tests_properties(shark.tooling.headless-environment PROPERTIES LABELS "cpu;tooling;server" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-evidence
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_probe.py" -v)
set_tests_properties(shark.tooling.graphics-evidence PROPERTIES LABELS "cpu;tooling" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

set(shark_storage_fixture_args)
if(TARGET EngineStorage)
    list(APPEND shark_storage_fixture_args --sqlite-archive "${SHARK_SQLITE_ARCHIVE_INPUT}")
    set(shark_storage_json_fixture_args)
    if(TARGET EngineTools)
        list(APPEND shark_storage_json_fixture_args --json-archive "${SHARK_JSON_ARCHIVE_INPUT}")
    endif()
    add_test(NAME shark.tooling.storage-dependencies
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_storage_dependencies.py"
            --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}"
            --compiler "${CMAKE_CXX_COMPILER}" --c-compiler "${CMAKE_C_COMPILER}"
            --entt-archive "${SHARK_EnTT_ARCHIVE}" --glm-archive "${SHARK_GLM_ARCHIVE_INPUT}"
            --mbedtls-archive "${SHARK_MbedTLS_ARCHIVE_INPUT}"
            --sqlite-archive "${SHARK_SQLITE_ARCHIVE_INPUT}"
            ${shark_storage_json_fixture_args} -v)
    set_tests_properties(shark.tooling.storage-dependencies PROPERTIES
        LABELS "cpu;tooling;boundary;server" TIMEOUT 240 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

add_test(NAME shark.tooling.graphics-dependencies
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_dependencies.py"
        --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}"
        --compiler "${CMAKE_CXX_COMPILER}" --entt-archive "${SHARK_EnTT_ARCHIVE}"
        --glm-archive "${SHARK_GLM_ARCHIVE_INPUT}" --c-compiler "${CMAKE_C_COMPILER}"
        --mbedtls-archive "${SHARK_MbedTLS_ARCHIVE_INPUT}" ${shark_storage_fixture_args} -v)
set_tests_properties(shark.tooling.graphics-dependencies PROPERTIES LABELS "cpu;tooling;boundary" TIMEOUT 240
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.scene-dependencies
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_scene_dependencies.py"
        --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}"
        --compiler "${CMAKE_CXX_COMPILER}" --entt-archive "${SHARK_EnTT_ARCHIVE}"
        --glm-archive "${SHARK_GLM_ARCHIVE_INPUT}" --c-compiler "${CMAKE_C_COMPILER}"
        --mbedtls-archive "${SHARK_MbedTLS_ARCHIVE_INPUT}" ${shark_storage_fixture_args} -v)
set_tests_properties(shark.tooling.scene-dependencies PROPERTIES LABELS "cpu;tooling;boundary;server" TIMEOUT 240
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

if(TARGET EngineTools)
    add_test(NAME shark.tooling.source-document-dependencies
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_source_document_dependencies.py"
            --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}"
            --compiler "${CMAKE_CXX_COMPILER}" --c-compiler "${CMAKE_C_COMPILER}"
            --entt-archive "${SHARK_EnTT_ARCHIVE}" --glm-archive "${SHARK_GLM_ARCHIVE_INPUT}"
            --mbedtls-archive "${SHARK_MbedTLS_ARCHIVE_INPUT}" --json-archive "${SHARK_JSON_ARCHIVE_INPUT}"
            ${shark_storage_fixture_args} -v)
    set_tests_properties(shark.tooling.source-document-dependencies PROPERTIES
        LABELS "cpu;tooling;boundary" TIMEOUT 240 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

add_test(NAME shark.tooling.graphics-tools
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_tools.py" -v)
set_tests_properties(shark.tooling.graphics-tools PROPERTIES LABELS "cpu;tooling" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-validation
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_validation.py" -v)
set_tests_properties(shark.tooling.graphics-validation PROPERTIES LABELS "cpu;tooling" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-package
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_package.py" -v)
set_tests_properties(shark.tooling.graphics-package PROPERTIES LABELS "cpu;tooling;boundary" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-package-linux
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_package_linux.py" -v)
set_tests_properties(shark.tooling.graphics-package-linux PROPERTIES LABELS "cpu;tooling;boundary" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
