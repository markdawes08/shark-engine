include(FetchContent)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/dependencies.lock.json")
file(READ "${PROJECT_SOURCE_DIR}/dependencies.lock.json" shark_dependency_lock)
string(JSON shark_lock_schema GET "${shark_dependency_lock}" schema_version)
if(NOT shark_lock_schema EQUAL 1)
    message(FATAL_ERROR "Unsupported dependencies.lock.json schema: ${shark_lock_schema}")
endif()
string(JSON shark_catch_url GET "${shark_dependency_lock}" dependencies Catch2 url)
string(JSON shark_catch_sha GET "${shark_dependency_lock}" dependencies Catch2 sha256)
string(LENGTH "${shark_catch_sha}" shark_sha_length)
if(NOT shark_sha_length EQUAL 64 OR NOT shark_catch_sha MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Catch2 must have an exact SHA-256 archive pin")
endif()
if(DEFINED FETCHCONTENT_SOURCE_DIR_CATCH2 AND NOT FETCHCONTENT_SOURCE_DIR_CATCH2 STREQUAL "")
    message(FATAL_ERROR "Unverified Catch2 source overrides are disabled; use the locked archive/cache")
endif()
string(JSON shark_option_count LENGTH "${shark_dependency_lock}" dependencies Catch2 options)
math(EXPR shark_option_last "${shark_option_count} - 1")
foreach(shark_option_index RANGE ${shark_option_last})
    string(JSON shark_option MEMBER "${shark_dependency_lock}" dependencies Catch2 options ${shark_option_index})
    string(JSON shark_value GET "${shark_dependency_lock}" dependencies Catch2 options "${shark_option}")
    set("${shark_option}" "${shark_value}" CACHE BOOL "Locked Catch2 option" FORCE)
endforeach()
FetchContent_Declare(Catch2
    URL "${shark_catch_url}"
    URL_HASH "SHA256=${shark_catch_sha}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(Catch2)
string(JSON shark_catch_cxx_standard GET "${shark_dependency_lock}" dependencies Catch2 cxx_standard)
foreach(shark_catch_target IN ITEMS Catch2 Catch2WithMain)
    # Catch's public headers detect standard-library features from the consumer
    # language mode. Its compiled library must use the same C++20 mode as Tests.
    target_compile_features(${shark_catch_target} PUBLIC "cxx_std_${shark_catch_cxx_standard}")
    set_target_properties(${shark_catch_target} PROPERTIES
        CXX_STANDARD "${shark_catch_cxx_standard}" CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    shark_enable_sanitizers(${shark_catch_target})
    shark_classify_target(${shark_catch_target} test)
    shark_classify_target(Catch2::${shark_catch_target} test)
endforeach()
