# The graphics experiment is opt-in and never reached by Core/Server builds.
include(FetchContent)

function(shark_validate_graphics_dependency name)
    string(TOUPPER "${name}" shark_upper_name)
    if(DEFINED FETCHCONTENT_SOURCE_DIR_${shark_upper_name}
            AND NOT FETCHCONTENT_SOURCE_DIR_${shark_upper_name} STREQUAL "")
        message(FATAL_ERROR "Unverified ${name} source overrides are disabled; use the locked archive/cache")
    endif()
    string(JSON shark_url GET "${shark_graphics_lock}" dependencies "${name}" url)
    string(JSON shark_sha GET "${shark_graphics_lock}" dependencies "${name}" sha256)
    string(LENGTH "${shark_sha}" shark_sha_length)
    if(NOT shark_sha_length EQUAL 64 OR NOT shark_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${name} must have an exact SHA-256 archive pin")
    endif()
endfunction()

function(shark_validate_graphics_inputs)
    file(READ "${PROJECT_SOURCE_DIR}/dependencies.lock.json" shark_graphics_lock)
    string(JSON shark_lock_schema GET "${shark_graphics_lock}" schema_version)
    if(NOT shark_lock_schema EQUAL 1)
        message(FATAL_ERROR "Unsupported dependencies.lock.json schema: ${shark_lock_schema}")
    endif()
    # Check every requested input before any FetchContent call, including Catch2.
    foreach(shark_name IN ITEMS VulkanHeaders VulkanLoader GLFW NVRHI)
        shark_validate_graphics_dependency("${shark_name}")
    endforeach()
endfunction()

function(shark_graphics_locked_dependency name)
    string(JSON shark_url GET "${shark_graphics_lock}" dependencies "${name}" url)
    string(JSON shark_sha GET "${shark_graphics_lock}" dependencies "${name}" sha256)
    string(JSON shark_option_count LENGTH "${shark_graphics_lock}" dependencies "${name}" options)
    math(EXPR shark_option_last "${shark_option_count} - 1")
    foreach(shark_option_index RANGE ${shark_option_last})
        string(JSON shark_option MEMBER "${shark_graphics_lock}" dependencies "${name}" options ${shark_option_index})
        string(JSON shark_value GET "${shark_graphics_lock}" dependencies "${name}" options "${shark_option}")
        if(shark_value MATCHES "^(ON|OFF)$")
            set("${shark_option}" "${shark_value}" CACHE BOOL "Locked ${name} option" FORCE)
        else()
            set("${shark_option}" "${shark_value}" CACHE STRING "Locked ${name} option" FORCE)
        endif()
    endforeach()
    string(JSON shark_platform_options ERROR_VARIABLE shark_no_platform_options
        GET "${shark_graphics_lock}" dependencies "${name}" platform_options "${CMAKE_SYSTEM_NAME}")
    if(NOT shark_no_platform_options)
        string(JSON shark_platform_count LENGTH "${shark_platform_options}")
        math(EXPR shark_platform_last "${shark_platform_count} - 1")
        foreach(shark_option_index RANGE ${shark_platform_last})
            string(JSON shark_option MEMBER "${shark_platform_options}" ${shark_option_index})
            string(JSON shark_value GET "${shark_platform_options}" "${shark_option}")
            set("${shark_option}" "${shark_value}" CACHE BOOL "Locked ${name} platform option" FORCE)
        endforeach()
    endif()
    # The redirect also prevents Vulkan-Loader's find_package(VulkanHeaders)
    # from finding an arbitrary installed SDK. No source tree/Git fallback exists.
    FetchContent_Declare(${name}
        URL "${shark_url}"
        URL_HASH "SHA256=${shark_sha}"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        OVERRIDE_FIND_PACKAGE
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(${name})
    string(TOLOWER "${name}" shark_lower_name)
    set("SHARK_${name}_SOURCE_DIR" "${${shark_lower_name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(shark_classify_graphics_directory directory)
    get_property(shark_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(shark_target IN LISTS shark_targets)
        shark_classify_target("${shark_target}" client)
    endforeach()
    get_property(shark_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(shark_directory IN LISTS shark_directories)
        shark_classify_graphics_directory("${shark_directory}")
    endforeach()
endfunction()

function(shark_setup_graphics_dependencies)
    if(NOT SHARK_BUILD_CLIENT OR NOT SHARK_BUILD_GRAPHICS_PROBE)
        message(FATAL_ERROR "Graphics dependencies require the explicit client probe configuration")
    endif()
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux|Darwin)$")
        message(FATAL_ERROR "The graphics probe has presets only for Windows, Linux and macOS")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/dependencies.lock.json")
    file(READ "${PROJECT_SOURCE_DIR}/dependencies.lock.json" shark_graphics_lock)
    shark_validate_graphics_inputs()

    shark_graphics_locked_dependency(VulkanHeaders)
    string(JSON shark_vulkan_version GET "${shark_graphics_lock}" dependencies VulkanHeaders version)
    # FetchContent's default package redirect has no version. Supply the exact
    # locked version required by Vulkan-Loader instead of probing the host SDK.
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file(
        "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/vulkanheaders-config-version.cmake"
        VERSION "${shark_vulkan_version}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
    shark_graphics_locked_dependency(VulkanLoader)
    shark_graphics_locked_dependency(GLFW)
    shark_graphics_locked_dependency(NVRHI)

    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan ALIAS vulkan)
    endif()
    # Keep the pinned loader beside the probe on Windows, and use CMake's build
    # RPATH for the same source-built loader on Linux/macOS.
    set_target_properties(vulkan PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_library(SharkGraphicsDependencies INTERFACE)
    target_link_libraries(SharkGraphicsDependencies INTERFACE
        glfw nvrhi nvrhi_vk Vulkan::Headers Vulkan::Vulkan)
    # Vulkan-Hpp's dispatcher layout depends on beta extension declarations.
    # Compile the backend and its consumer with the same locked definitions.
    string(JSON shark_definition_count LENGTH "${shark_graphics_lock}"
        dependencies NVRHI vulkan_compile_definitions)
    math(EXPR shark_definition_last "${shark_definition_count} - 1")
    foreach(shark_definition_index RANGE ${shark_definition_last})
        string(JSON shark_definition GET "${shark_graphics_lock}"
            dependencies NVRHI vulkan_compile_definitions ${shark_definition_index})
        target_compile_definitions(nvrhi_vk PRIVATE "${shark_definition}")
        target_compile_definitions(SharkGraphicsDependencies INTERFACE "${shark_definition}")
    endforeach()
    shark_classify_target(SharkGraphicsDependencies client)
    foreach(shark_name IN ITEMS VulkanHeaders VulkanLoader GLFW NVRHI)
        shark_classify_graphics_directory("${SHARK_${shark_name}_SOURCE_DIR}")
    endforeach()
    foreach(shark_alias IN ITEMS Vulkan::Headers Vulkan::Loader Vulkan::Vulkan)
        shark_classify_target("${shark_alias}" client)
    endforeach()
    message(STATUS "Graphics dependencies use locked sources; hardware qualification remains assignment 02")
endfunction()
