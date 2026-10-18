cmake_minimum_required(VERSION 3.28)
foreach(shark_case IN ITEMS allowed server-only security-flags security-flag-injection client-link client-genex authoring-genex
        runtime-authoring raw-link link-option link-directory prebuilt-object utility-client utility-authoring utility-unclassified
        include-client include-authoring source-client source-authoring)
    set(shark_build "${SHARK_FIXTURE_BINARY}/${shark_case}")
    set(shark_fixture "${SHARK_SOURCE}/tests/boundary_fixture")
    if(shark_case STREQUAL "server-only")
        string(APPEND shark_fixture "/server_only")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${shark_fixture}" -B "${shark_build}" -G Ninja
        "-DSHARK_SOURCE=${SHARK_SOURCE}" "-DSHARK_BOUNDARY_CASE=${shark_case}"
        RESULT_VARIABLE shark_configure OUTPUT_VARIABLE shark_output ERROR_VARIABLE shark_error TIMEOUT 10)
    if(NOT shark_configure EQUAL 0)
        message(FATAL_ERROR "Boundary fixture ${shark_case} configuration failed: ${shark_output} ${shark_error}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSHARK_GRAPH=${shark_build}/shark-target-graph-.cmake"
        -P "${SHARK_SOURCE}/cmake/CheckRuntimeBoundary.cmake"
        RESULT_VARIABLE shark_result OUTPUT_VARIABLE shark_output ERROR_VARIABLE shark_error TIMEOUT 5)
    if(shark_case MATCHES "^(allowed|server-only|security-flags)$")
        if(NOT shark_result EQUAL 0)
            message(FATAL_ERROR "Allowed ${shark_case} closure failed: ${shark_output} ${shark_error}")
        endif()
    elseif(shark_result EQUAL 0 OR NOT shark_error MATCHES "reaches (forbidden|unclassified)")
        message(FATAL_ERROR "Violation ${shark_case} was not rejected by the boundary checker: ${shark_output} ${shark_error}")
    endif()
endforeach()
message(STATUS "Boundary fixtures: 3 allowed configurations and 16 forbidden dependency paths verified")
