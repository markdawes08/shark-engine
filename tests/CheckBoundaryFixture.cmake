cmake_minimum_required(VERSION 3.28)
foreach(shark_violation IN ITEMS OFF ON)
    set(shark_build "${SHARK_FIXTURE_BINARY}/${shark_violation}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${SHARK_SOURCE}/tests/boundary_fixture" -B "${shark_build}" -G Ninja
        "-DSHARK_SOURCE=${SHARK_SOURCE}" "-DSHARK_BOUNDARY_VIOLATION=${shark_violation}"
        RESULT_VARIABLE shark_configure OUTPUT_VARIABLE shark_output ERROR_VARIABLE shark_error TIMEOUT 10)
    if(NOT shark_configure EQUAL 0)
        message(FATAL_ERROR "Boundary fixture configuration failed: ${shark_output} ${shark_error}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSHARK_GRAPH=${shark_build}/shark-target-graph-.cmake"
        -P "${SHARK_SOURCE}/cmake/CheckRuntimeBoundary.cmake"
        RESULT_VARIABLE shark_result OUTPUT_VARIABLE shark_output ERROR_VARIABLE shark_error TIMEOUT 5)
    if(shark_violation STREQUAL "OFF")
        if(NOT shark_result EQUAL 0)
            message(FATAL_ERROR "Allowed runtime closure failed: ${shark_output} ${shark_error}")
        endif()
    elseif(shark_result EQUAL 0 OR NOT shark_error MATCHES "forbidden authoring target 'EngineTools'")
        message(FATAL_ERROR "Transitive generator-expression violation was not detected: ${shark_output} ${shark_error}")
    endif()
endforeach()
