# Every linked target is assigned an architectural domain. Generator expressions
# are evaluated by CMake itself; the checker does not parse their syntax.
function(shark_classify_target target domain)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot classify missing target '${target}'")
    endif()
    if(NOT domain MATCHES "^(runtime|authoring|test)$")
        message(FATAL_ERROR "Unknown target domain '${domain}'")
    endif()
    set_property(GLOBAL APPEND PROPERTY SHARK_CLASSIFIED_TARGETS "${target}")
    set_property(GLOBAL PROPERTY "SHARK_DOMAIN_${target}" "${domain}")
endfunction()

function(shark_generate_boundary_check)
    get_property(shark_targets GLOBAL PROPERTY SHARK_CLASSIFIED_TARGETS)
    list(REMOVE_DUPLICATES shark_targets)
    set(shark_graph "set(SHARK_GRAPH_TARGETS \"${shark_targets}\")\n")
    foreach(shark_target IN LISTS shark_targets)
        get_property(shark_domain GLOBAL PROPERTY "SHARK_DOMAIN_${shark_target}")
        string(APPEND shark_graph "set(\"SHARK_DOMAIN_${shark_target}\" \"${shark_domain}\")\n")
        string(APPEND shark_graph
            "set(\"SHARK_LINKS_${shark_target}\" \"$<TARGET_GENEX_EVAL:${shark_target},$<TARGET_PROPERTY:${shark_target},LINK_LIBRARIES>>;$<TARGET_GENEX_EVAL:${shark_target},$<TARGET_PROPERTY:${shark_target},INTERFACE_LINK_LIBRARIES>>\")\n")
    endforeach()
    set(shark_graph_path "${CMAKE_BINARY_DIR}/shark-target-graph-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${shark_graph_path}" CONTENT "${shark_graph}")
    set(shark_checker "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckRuntimeBoundary.cmake")
    add_custom_target(shark_check_runtime_boundary ALL
        COMMAND "${CMAKE_COMMAND}" "-DSHARK_GRAPH=${shark_graph_path}" -P "${shark_checker}"
        COMMENT "Checking Runtime and Engine transitive dependency boundaries"
        VERBATIM)
    add_dependencies(Runtime shark_check_runtime_boundary)
    if(SHARK_BUILD_TESTS)
        add_test(NAME shark.boundary
            COMMAND "${CMAKE_COMMAND}" "-DSHARK_GRAPH=${shark_graph_path}" -P "${shark_checker}")
        set_tests_properties(shark.boundary PROPERTIES LABELS "cpu;boundary" TIMEOUT 10)
    endif()
endfunction()
