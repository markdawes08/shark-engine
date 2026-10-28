add_library(EngineClient STATIC
    src/bootstrap.cpp src/input.cpp src/frame_loop.cpp src/window.cpp src/window_events.cpp src/window_launch.cpp
    src/graphics.cpp src/gpu_resources.cpp src/shader_bundle.cpp src/cube_sample.cpp src/scene_viewport.cpp
    src/presentation.cpp src/presentation_launch.cpp)
if(WIN32)
    target_sources(EngineClient PRIVATE src/graphics_queue_waiter_win32.cpp)
endif()
if(SHARK_NATIVE_WINDOW)
    target_sources(EngineClient PRIVATE src/window_glfw.cpp)
    target_link_libraries(EngineClient PRIVATE SharkWindowDependencies)
    # Establish the process DPI policy before DLL/UI initialization. Native
    # graphics consumers also need UTF-8 for the pinned loader's narrow paths.
    target_sources(EngineClient INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/windows.manifest")
else()
    target_sources(EngineClient PRIVATE src/window_unavailable.cpp)
endif()
if(SHARK_NATIVE_GRAPHICS)
    target_sources(EngineClient PRIVATE src/graphics_vulkan.cpp src/graphics_offscreen.cpp src/graphics_scene.cpp)
    target_link_libraries(EngineClient PRIVATE SharkGraphicsDependencies)
    target_compile_definitions(EngineClient PRIVATE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
    if(SHARK_BUILD_TESTS)
        # Explicit private qualification factories only; never enabled by an
        # application argument/environment setting or in Runtime-only builds.
        target_compile_definitions(EngineClient PRIVATE SHARK_GRAPHICS_TEST_HOOKS=1)
    endif()
else()
    target_sources(EngineClient PRIVATE src/graphics_unavailable.cpp)
endif()
target_include_directories(EngineClient PUBLIC include)
target_link_libraries(EngineClient PUBLIC EngineCore)
shark_configure_target(EngineClient)
shark_classify_target(EngineClient client)
