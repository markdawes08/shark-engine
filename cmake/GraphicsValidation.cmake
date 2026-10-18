# Standalone development-only validation tool graph. Preparation verifies every
# source byte against the locked archives before invoking this project.
function(shark_setup_graphics_validation)
    if(NOT PROJECT_NAME STREQUAL "SharkGraphicsValidation"
            OR NOT PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "Graphics validation tools require their standalone project")
    endif()
    if(NOT SHARK_VALIDATION_SOURCE_ROOT)
        message(FATAL_ERROR "Use tools/prepare_graphics_validation.py to verify locked inputs first")
    endif()
    file(TO_CMAKE_PATH "${SHARK_VALIDATION_SOURCE_ROOT}" SHARK_VALIDATION_SOURCE_ROOT)
    get_filename_component(shark_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${shark_root}/tools/prepare_graphics_validation.py"
            --verify-cache "${SHARK_VALIDATION_SOURCE_ROOT}"
        RESULT_VARIABLE shark_source_result
        ERROR_VARIABLE shark_source_error
        OUTPUT_VARIABLE shark_source_output
        TIMEOUT 60)
    if(NOT shark_source_result EQUAL 0)
        message(FATAL_ERROR "Validation source integrity failed: ${shark_source_error}${shark_source_output}")
    endif()
    file(READ "${shark_root}/dependencies.lock.json" shark_lock)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${shark_root}/dependencies.lock.json")
    string(JSON shark_configuration GET "${shark_lock}" graphics_validation build_configuration)
    if(NOT CMAKE_BUILD_TYPE STREQUAL shark_configuration)
        message(FATAL_ERROR "Validation tools require the locked ${shark_configuration} configuration")
    endif()
    set(CMAKE_DISABLE_FIND_PACKAGE_mimalloc ON CACHE BOOL "No ambient allocator" FORCE)
    set(CMAKE_DISABLE_FIND_PACKAGE_phmap ON CACHE BOOL "Use exact vendored headers" FORCE)
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
    set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    include(CMakePackageConfigHelpers)
    foreach(shark_name IN ITEMS VulkanHeaders SPIRVHeaders SPIRVTools
            VulkanUtilityLibraries VulkanValidationLayers)
        if(shark_name STREQUAL "VulkanHeaders")
            string(JSON shark_entry GET "${shark_lock}" dependencies "${shark_name}")
        else()
            string(JSON shark_entry GET "${shark_lock}" graphics_validation dependencies "${shark_name}")
        endif()
        string(JSON shark_sha GET "${shark_entry}" sha256)
        string(SUBSTRING "${shark_sha}" 0 16 shark_short_sha)
        set(shark_source "${SHARK_VALIDATION_SOURCE_ROOT}/${shark_name}-${shark_short_sha}")
        if(NOT EXISTS "${shark_source}/CMakeLists.txt")
            message(FATAL_ERROR "Missing verified source for ${shark_name}; run preparation")
        endif()
        string(JSON shark_option_count LENGTH "${shark_entry}" options)
        math(EXPR shark_option_last "${shark_option_count} - 1")
        foreach(shark_index RANGE ${shark_option_last})
            string(JSON shark_option MEMBER "${shark_entry}" options ${shark_index})
            string(JSON shark_value GET "${shark_entry}" options "${shark_option}")
            set("${shark_option}" "${shark_value}" CACHE BOOL "Locked ${shark_name} option" FORCE)
        endforeach()
        # Package requests resolve only to targets from this exact source graph.
        # Empty configs are sufficient because add_subdirectory creates targets.
        if(shark_name STREQUAL "SPIRVHeaders")
            set(shark_package "SPIRV-Headers")
            set(SPIRV-Headers_SOURCE_DIR "${shark_source}")
        elseif(shark_name STREQUAL "SPIRVTools")
            set(shark_package "SPIRV-Tools-opt")
        else()
            set(shark_package "${shark_name}")
        endif()
        file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/${shark_package}Config.cmake"
            "# Shark's verified validation source graph owns this target.\n")
        if(shark_name STREQUAL "VulkanHeaders")
            string(JSON VulkanHeaders_VERSION GET "${shark_entry}" version)
            write_basic_package_version_file(
                "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/${shark_package}ConfigVersion.cmake"
                VERSION "${VulkanHeaders_VERSION}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
        endif()
        add_subdirectory("${shark_source}" "${CMAKE_BINARY_DIR}/deps/${shark_name}" EXCLUDE_FROM_ALL)
    endforeach()
    set_target_properties(spirv-val vvl PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Keep the locked RelWithDebInfo compilation and its debug object files,
        # but omit non-runtime debug sections from the distributed ELF layer.
        # The unstripped layer exceeds the bounded portable package file size.
        target_link_options(vvl PRIVATE "LINKER:--strip-debug")
    endif()
    add_custom_target(shark_graphics_validation DEPENDS spirv-val vvl)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/validation-products.json" CONTENT
        "{\n  \"spirv_val\": \"$<TARGET_FILE:spirv-val>\",\n  \"validation_library\": \"$<TARGET_FILE:vvl>\",\n  \"validation_manifest\": \"$<TARGET_FILE_DIR:vvl>/VkLayer_khronos_validation.json\",\n  \"compiler\": \"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\"\n}\n")
endfunction()
