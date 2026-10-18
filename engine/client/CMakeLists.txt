add_library(EngineClient STATIC
    src/bootstrap.cpp src/input.cpp src/frame_loop.cpp src/window.cpp src/window_events.cpp src/window_launch.cpp
    src/graphics.cpp)
if(SHARK_NATIVE_WINDOW)
    target_sources(EngineClient PRIVATE src/window_glfw.cpp)
    target_link_libraries(EngineClient PRIVATE SharkWindowDependencies)
else()
    target_sources(EngineClient PRIVATE src/window_unavailable.cpp)
endif()
if(SHARK_NATIVE_GRAPHICS)
    target_sources(EngineClient PRIVATE src/graphics_vulkan.cpp)
    target_link_libraries(EngineClient PRIVATE SharkGraphicsDependencies)
    target_compile_definitions(EngineClient PRIVATE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
    # The pinned Vulkan loader uses UTF-8 with narrow Windows/CRT paths. Apply
    # the process code page before startup for every native client consumer.
    target_sources(EngineClient INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/windows.manifest")
else()
    target_sources(EngineClient PRIVATE src/graphics_unavailable.cpp)
endif()
target_include_directories(EngineClient PUBLIC include)
target_link_libraries(EngineClient PUBLIC EngineCore)
shark_configure_target(EngineClient)
shark_classify_target(EngineClient client)
