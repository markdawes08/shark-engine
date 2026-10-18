add_subdirectory(core)

# Gate the entire presentation subtree before dependency discovery/downloads.
if(SHARK_BUILD_CLIENT)
    add_subdirectory(client)
    add_library(Engine INTERFACE)
    target_link_libraries(Engine INTERFACE EngineClient)
    shark_classify_target(Engine client)
endif()
