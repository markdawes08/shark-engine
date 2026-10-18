# Shared locked source handling for the native window and opt-in graphics probe.
include_guard(GLOBAL)
include(FetchContent)

function(shark_read_client_dependency_lock output)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/dependencies.lock.json")
    file(READ "${PROJECT_SOURCE_DIR}/dependencies.lock.json" shark_client_lock)
    string(JSON shark_lock_schema GET "${shark_client_lock}" schema_version)
    if(NOT shark_lock_schema EQUAL 1)
        message(FATAL_ERROR "Unsupported dependencies.lock.json schema: ${shark_lock_schema}")
    endif()
    set("${output}" "${shark_client_lock}" PARENT_SCOPE)
endfunction()

function(shark_validate_client_dependency name)
    string(TOUPPER "${name}" shark_upper_name)
    if(DEFINED FETCHCONTENT_SOURCE_DIR_${shark_upper_name}
            AND NOT FETCHCONTENT_SOURCE_DIR_${shark_upper_name} STREQUAL "")
        message(FATAL_ERROR "Unverified ${name} source overrides are disabled; use the locked archive/cache")
    endif()
    string(JSON shark_url GET "${shark_client_lock}" dependencies "${name}" url)
    string(JSON shark_sha GET "${shark_client_lock}" dependencies "${name}" sha256)
    string(LENGTH "${shark_sha}" shark_sha_length)
    if(NOT shark_sha_length EQUAL 64 OR NOT shark_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${name} must have an exact SHA-256 archive pin")
    endif()
endfunction()

function(shark_client_locked_dependency name)
    shark_validate_client_dependency("${name}")
    string(JSON shark_url GET "${shark_client_lock}" dependencies "${name}" url)
    string(JSON shark_sha GET "${shark_client_lock}" dependencies "${name}" sha256)
    string(JSON shark_option_count LENGTH "${shark_client_lock}" dependencies "${name}" options)
    if(shark_option_count GREATER 0)
        math(EXPR shark_option_last "${shark_option_count} - 1")
        foreach(shark_option_index RANGE ${shark_option_last})
            string(JSON shark_option MEMBER "${shark_client_lock}" dependencies "${name}" options ${shark_option_index})
            string(JSON shark_value GET "${shark_client_lock}" dependencies "${name}" options "${shark_option}")
            if(shark_value MATCHES "^(ON|OFF)$")
                set("${shark_option}" "${shark_value}" CACHE BOOL "Locked ${name} option" FORCE)
            else()
                set("${shark_option}" "${shark_value}" CACHE STRING "Locked ${name} option" FORCE)
            endif()
        endforeach()
    endif()
    string(JSON shark_platform_options ERROR_VARIABLE shark_no_platform_options
        GET "${shark_client_lock}" dependencies "${name}" platform_options "${CMAKE_SYSTEM_NAME}")
    if(NOT shark_no_platform_options)
        string(JSON shark_platform_count LENGTH "${shark_platform_options}")
        if(shark_platform_count GREATER 0)
            math(EXPR shark_platform_last "${shark_platform_count} - 1")
            foreach(shark_option_index RANGE ${shark_platform_last})
                string(JSON shark_option MEMBER "${shark_platform_options}" ${shark_option_index})
                string(JSON shark_value GET "${shark_platform_options}" "${shark_option}")
                set("${shark_option}" "${shark_value}" CACHE BOOL "Locked ${name} platform option" FORCE)
            endforeach()
        endif()
    endif()
    # The redirect prevents a dependency's find_package from selecting an
    # arbitrary installed SDK. No source tree/Git fallback exists.
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

function(shark_classify_client_dependency_directory directory)
    get_property(shark_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(shark_target IN LISTS shark_targets)
        shark_classify_target("${shark_target}" client)
    endforeach()
    get_property(shark_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(shark_directory IN LISTS shark_directories)
        shark_classify_client_dependency_directory("${shark_directory}")
    endforeach()
endfunction()
