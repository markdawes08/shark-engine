# CMake evaluates generator expressions; every reached link/build utility needs a domain.
function(shark_classify_target target domain)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot classify missing target '${target}'")
    endif()
    if(NOT domain MATCHES "^(core|client|runtime|authoring|test)$")
        message(FATAL_ERROR "Unknown target domain '${domain}'")
    endif()
    get_property(shark_previous GLOBAL PROPERTY "SHARK_DOMAIN_${target}")
    if(shark_previous AND NOT shark_previous STREQUAL domain)
        message(FATAL_ERROR "Cannot reclassify '${target}' from ${shark_previous} to ${domain}")
    endif()
    set_property(GLOBAL APPEND PROPERTY SHARK_CLASSIFIED_TARGETS "${target}")
    set_property(GLOBAL PROPERTY "SHARK_DOMAIN_${target}" "${domain}")
endfunction()

function(shark_generate_boundary_check)
    set(shark_graph_path "${CMAKE_BINARY_DIR}/shark-target-graph-$<CONFIG>.cmake")
    set(shark_checker "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckRuntimeBoundary.cmake")
    if(TARGET Runtime)
        add_custom_target(shark_check_runtime_boundary ALL
            COMMAND "${CMAKE_COMMAND}" "-DSHARK_GRAPH=${shark_graph_path}"
                -DSHARK_BOUNDARY_MODE=client -P "${shark_checker}"
            COMMENT "Checking client transitive dependency boundaries" VERBATIM)
        shark_classify_target(shark_check_runtime_boundary client)
        add_dependencies(Runtime shark_check_runtime_boundary)
    endif()
    if(TARGET EngineCore OR TARGET Server)
        add_custom_target(shark_check_server_boundary ALL
            COMMAND "${CMAKE_COMMAND}" "-DSHARK_GRAPH=${shark_graph_path}"
                -DSHARK_BOUNDARY_MODE=server -P "${shark_checker}"
            COMMENT "Checking headless Server/Core dependency boundaries" VERBATIM)
        shark_classify_target(shark_check_server_boundary core)
        if(TARGET Server)
            add_dependencies(Server shark_check_server_boundary)
        endif()
    endif()
    get_property(shark_targets GLOBAL PROPERTY SHARK_CLASSIFIED_TARGETS)
    list(REMOVE_DUPLICATES shark_targets)
    set(shark_graph "set(SHARK_GRAPH_TARGETS [==[${shark_targets}]==])\n")
    string(APPEND shark_graph
        "set(SHARK_SOURCE_ROOT [==[${CMAKE_SOURCE_DIR}]==])\n"
        "set(SHARK_BUILD_ROOT [==[${CMAKE_BINARY_DIR}]==])\n")
    foreach(shark_target IN LISTS shark_targets)
        get_property(shark_domain GLOBAL PROPERTY "SHARK_DOMAIN_${shark_target}")
        get_target_property(shark_source_dir "${shark_target}" SOURCE_DIR)
        string(APPEND shark_graph
            "set(SHARK_DOMAIN_${shark_target} [==[${shark_domain}]==])\n"
            "set(SHARK_SOURCE_DIR_${shark_target} [==[${shark_source_dir}]==])\n")
        foreach(shark_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES LINK_OPTIONS INTERFACE_LINK_OPTIONS
                LINK_DIRECTORIES INTERFACE_LINK_DIRECTORIES
                MANUALLY_ADDED_DEPENDENCIES SOURCES INTERFACE_SOURCES
                INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
            string(APPEND shark_graph
                "set(SHARK_${shark_property}_${shark_target} [==[$<TARGET_GENEX_EVAL:${shark_target},$<TARGET_PROPERTY:${shark_target},${shark_property}>>]==])\n")
        endforeach()
    endforeach()
    file(GENERATE OUTPUT "${shark_graph_path}" CONTENT "${shark_graph}")
    if(SHARK_BUILD_TESTS)
        add_test(NAME shark.boundary
            COMMAND "${CMAKE_COMMAND}" "-DSHARK_GRAPH=${shark_graph_path}" -P "${shark_checker}")
        set_tests_properties(shark.boundary PROPERTIES LABELS "cpu;boundary" TIMEOUT 10)
    endif()
endfunction()
