add_library(EngineStorage STATIC storage/src/store.cpp storage/src/characters.cpp storage/src/movement.cpp
    storage/src/durable_worker.cpp storage/src/combat.cpp storage/src/regions.cpp
    storage/src/execution_fence.cpp storage/src/gameplay_access.cpp
    storage/src/region_image.cpp storage/src/region_executor.cpp
    storage/src/region_journal.cpp storage/src/region_child.cpp
    storage/src/region_process.cpp storage/src/region_gateway.cpp)
target_include_directories(EngineStorage PUBLIC storage/include)
target_link_libraries(EngineStorage PUBLIC EngineCore PRIVATE SharkSQLite)
shark_configure_target(EngineStorage)
shark_classify_target(EngineStorage storage)

add_executable(Server main.cpp)
target_link_libraries(Server PRIVATE EngineStorage)
shark_configure_target(Server)
shark_classify_target(Server storage)
