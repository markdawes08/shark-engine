add_executable(GraphicsProbe main.cpp)
target_link_libraries(GraphicsProbe PRIVATE SharkGraphicsDependencies)
shark_configure_target(GraphicsProbe)
shark_classify_target(GraphicsProbe client)

file(SHA256 "${PROJECT_SOURCE_DIR}/dependencies.lock.json" shark_probe_lock_sha256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/dependencies.lock.json")
set(shark_probe_source_material "")
foreach(shark_probe_source IN ITEMS
        probes/graphics/CMakeLists.txt probes/graphics/main.cpp probes/graphics/report.hpp)
    file(SHA256 "${PROJECT_SOURCE_DIR}/${shark_probe_source}" shark_probe_file_sha256)
    string(APPEND shark_probe_source_material "${shark_probe_source}\n${shark_probe_file_sha256}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/${shark_probe_source}")
endforeach()
string(SHA256 shark_probe_source_sha256 "${shark_probe_source_material}")
target_compile_definitions(GraphicsProbe PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    SHARK_PROBE_VERSION="${PROJECT_VERSION}"
    SHARK_PROBE_LOCK_SHA256="${shark_probe_lock_sha256}"
    SHARK_PROBE_SOURCE_SHA256="${shark_probe_source_sha256}"
    SHARK_PROBE_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
if(WIN32)
    target_compile_definitions(GraphicsProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(SHARK_BUILD_TESTS)
    set(Python3_FIND_STRATEGY LOCATION)
    set(Python3_FIND_REGISTRY NEVER)
    set(Python3_FIND_FRAMEWORK NEVER)
    find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
    target_sources(Tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/report_tests.cpp")
    add_test(NAME shark.graphics-probe.report COMMAND Tests "[graphics-probe]" --rng-seed 1337)
    set_tests_properties(shark.graphics-probe.report PROPERTIES
        LABELS "cpu;unit;graphics-probe" TIMEOUT 10)
    add_test(NAME shark.graphics-probe.cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_cli.py"
            --executable "$<TARGET_FILE:GraphicsProbe>"
            --lock "${PROJECT_SOURCE_DIR}/dependencies.lock.json")
    set_tests_properties(shark.graphics-probe.cli PROPERTIES
        LABELS "cpu;integration;graphics-probe" TIMEOUT 30)
endif()
