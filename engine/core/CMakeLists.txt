configure_file(../version_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version_config.hpp" @ONLY)
add_library(EngineCore STATIC
    src/bootstrap.cpp
    src/error.cpp
    src/log.cpp
    src/name.cpp
    src/project_path.cpp
    src/scene.cpp
    src/schema.cpp
    src/net_wire.cpp
    src/net_fault_link.cpp
    src/net_transport.cpp
    src/net_session.cpp
    src/net_session_wire.cpp
    src/net_replication.cpp
    src/net_replication_wire.cpp
    src/server_application.cpp
    src/server_config.cpp
    src/shutdown_signals.cpp
    src/stable_id.cpp
    src/time.cpp
    src/transform.cpp
    src/world_position.cpp)
target_include_directories(EngineCore PUBLIC ../include include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Standard-library sleeping and ordinary worker synchronization use the OS thread
# runtime. No fetched dependency, worker pool or presentation service is introduced.
find_package(Threads REQUIRED)
set_property(TARGET Threads::Threads PROPERTY IMPORTED_GLOBAL TRUE)
shark_classify_target(Threads::Threads core)
target_link_libraries(EngineCore PUBLIC Threads::Threads SharkMath)
target_link_libraries(EngineCore PRIVATE EnTT::EnTT SharkTLS_mbedtls)
if(WIN32)
    # Windows OS entropy for persistent UUIDs; no third-party or presentation dependency.
    find_library(SHARK_WINDOWS_BCRYPT_LIBRARY NAMES bcrypt REQUIRED)
    add_library(SharkWindowsEntropy STATIC IMPORTED GLOBAL)
    set_target_properties(SharkWindowsEntropy PROPERTIES
        IMPORTED_LOCATION "${SHARK_WINDOWS_BCRYPT_LIBRARY}")
    shark_classify_target(SharkWindowsEntropy core)
    target_link_libraries(EngineCore PRIVATE SharkWindowsEntropy)
endif()
shark_configure_target(EngineCore)
shark_classify_target(EngineCore core)
