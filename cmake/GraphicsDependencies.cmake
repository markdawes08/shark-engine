# The graphics experiment is opt-in and never reached by Core/Server builds.
include_guard(GLOBAL)
include(WindowDependencies)

function(shark_validate_graphics_inputs)
    shark_read_client_dependency_lock(shark_client_lock)
    # Check every requested input before any FetchContent call, including Catch2.
    foreach(shark_name IN ITEMS VulkanHeaders VulkanLoader GLFW NVRHI)
        shark_validate_client_dependency("${shark_name}")
    endforeach()
endfunction()

function(shark_setup_graphics_dependencies)
    if(NOT SHARK_BUILD_CLIENT OR NOT SHARK_BUILD_GRAPHICS_PROBE)
        message(FATAL_ERROR "Graphics dependencies require the explicit client probe configuration")
    endif()
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux)$")
        message(FATAL_ERROR "The graphics probe targets Windows and Linux only")
    endif()
    shark_read_client_dependency_lock(shark_client_lock)
    shark_validate_graphics_inputs()

    shark_client_locked_dependency(VulkanHeaders)
    string(JSON shark_vulkan_version GET "${shark_client_lock}" dependencies VulkanHeaders version)
    # FetchContent's default package redirect has no version. Supply the exact
    # locked version required by Vulkan-Loader instead of probing the host SDK.
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file(
        "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/vulkanheaders-config-version.cmake"
        VERSION "${shark_vulkan_version}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
    shark_client_locked_dependency(VulkanLoader)
    shark_setup_window_dependencies()
    shark_client_locked_dependency(NVRHI)

    # The pinned static Vulkan backend calls the common NVRHI implementation,
    # but upstream does not declare that link edge. Model it explicitly so ELF
    # linkers place the common archive after its backend consumers.
    target_link_libraries(nvrhi_vk PRIVATE nvrhi)

    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan ALIAS vulkan)
    endif()
    # Keep the pinned loader beside the probe on Windows, and use CMake's build
    # RPATH for the same source-built loader on Linux.
    set_target_properties(vulkan PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_library(SharkGraphicsDependencies INTERFACE)
    target_link_libraries(SharkGraphicsDependencies INTERFACE
        SharkWindowDependencies nvrhi nvrhi_vk Vulkan::Headers Vulkan::Vulkan)
    # Vulkan-Hpp's dispatcher layout depends on beta extension declarations.
    # Compile the backend and its consumer with the same locked definitions.
    string(JSON shark_definition_count LENGTH "${shark_client_lock}"
        dependencies NVRHI vulkan_compile_definitions)
    math(EXPR shark_definition_last "${shark_definition_count} - 1")
    foreach(shark_definition_index RANGE ${shark_definition_last})
        string(JSON shark_definition GET "${shark_client_lock}"
            dependencies NVRHI vulkan_compile_definitions ${shark_definition_index})
        target_compile_definitions(nvrhi_vk PRIVATE "${shark_definition}")
        target_compile_definitions(SharkGraphicsDependencies INTERFACE "${shark_definition}")
    endforeach()
    shark_classify_target(SharkGraphicsDependencies client)
    foreach(shark_name IN ITEMS VulkanHeaders VulkanLoader NVRHI)
        shark_classify_client_dependency_directory("${SHARK_${shark_name}_SOURCE_DIR}")
    endforeach()
    foreach(shark_alias IN ITEMS Vulkan::Headers Vulkan::Loader Vulkan::Vulkan)
        shark_classify_target("${shark_alias}" client)
    endforeach()
    message(STATUS "Graphics dependencies use locked sources; hardware qualification remains assignment 02")
endfunction()
