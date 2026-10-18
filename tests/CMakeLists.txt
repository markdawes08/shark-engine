add_executable(Tests
    bootstrap_tests.cpp
    core_error_tests.cpp
    core_identity_tests.cpp
    core_path_tests.cpp
    server_tests.cpp)
target_link_libraries(Tests PRIVATE EngineCore Catch2::Catch2WithMain)
target_compile_definitions(Tests PRIVATE
    SHARK_EXPECTED_VERSION="${PROJECT_VERSION}"
    SHARK_INJECT_TEST_FAILURE=$<BOOL:${SHARK_INJECT_TEST_FAILURE}>)
shark_configure_target(Tests)
shark_classify_target(Tests test)

# Explicit registration avoids executing a sanitizer binary during the build.
add_test(NAME shark.unit COMMAND Tests "[bootstrap]" --rng-seed 1337)
set_tests_properties(shark.unit PROPERTIES LABELS "cpu;unit" TIMEOUT 10)
add_test(NAME shark.failure-sentinel COMMAND Tests "[failure-sentinel]" --rng-seed 1337)
set_tests_properties(shark.failure-sentinel PROPERTIES LABELS "cpu;unit" TIMEOUT 10)
add_test(NAME shark.server.unit COMMAND Tests "[server]" --rng-seed 1337)
set_tests_properties(shark.server.unit PROPERTIES LABELS "cpu;server;unit" TIMEOUT 10)
add_test(NAME shark.core.contracts COMMAND Tests "[core]" --rng-seed 1337)
set_tests_properties(shark.core.contracts PROPERTIES LABELS "cpu;unit" TIMEOUT 15)

foreach(shark_application IN ITEMS Runtime Editor)
    if(TARGET "${shark_application}")
        string(TOLOWER "${shark_application}" shark_application_lower)
        add_test(NAME "shark.${shark_application_lower}.cli"
            COMMAND "${CMAKE_COMMAND}"
                "-DSHARK_EXECUTABLE=$<TARGET_FILE:${shark_application}>"
                "-DSHARK_APPLICATION=${shark_application}"
                "-DSHARK_VERSION=${PROJECT_VERSION}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCli.cmake")
        set_tests_properties("shark.${shark_application_lower}.cli" PROPERTIES
            LABELS "cpu;integration" TIMEOUT 30)
    endif()
endforeach()
add_test(NAME shark.boundary.fixture
    COMMAND "${CMAKE_COMMAND}"
        "-DSHARK_SOURCE=${PROJECT_SOURCE_DIR}"
        "-DSHARK_FIXTURE_BINARY=${CMAKE_CURRENT_BINARY_DIR}/boundary-fixture"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckBoundaryFixture.cmake")
set_tests_properties(shark.boundary.fixture PROPERTIES LABELS "cpu;boundary" TIMEOUT 30)

# Repository checks are supplied by the foundation tooling without making them
# dependencies of the runtime-only configuration.
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_REGISTRY NEVER)
set(Python3_FIND_FRAMEWORK NEVER)
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
if(TARGET Server)
    add_test(NAME shark.server.lifecycle
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_server.py"
            --executable "$<TARGET_FILE:Server>" --expected-version "${PROJECT_VERSION}")
    set_tests_properties(shark.server.lifecycle PROPERTIES LABELS "cpu;server;integration" TIMEOUT 60)
endif()
add_test(NAME shark.tooling.processes
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_process_runner.py" -v)
set_tests_properties(shark.tooling.processes PROPERTIES LABELS "cpu;tooling" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.tooling.sources
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_clean_source.py" -v)
set_tests_properties(shark.tooling.sources PROPERTIES LABELS "cpu;tooling" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.contract
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/validate_contract.py" --self-test)
set_tests_properties(shark.contract PROPERTIES LABELS "cpu;contract" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
if(EXISTS "${PROJECT_SOURCE_DIR}/tools/check_repository.py")
    add_test(NAME shark.repository
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_repository.py")
    set_tests_properties(shark.repository PROPERTIES LABELS "cpu;contract" TIMEOUT 30
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

add_test(NAME shark.tooling.server-closure
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_server_closure.py" -v)
set_tests_properties(shark.tooling.server-closure PROPERTIES
    LABELS "cpu;tooling;boundary;server" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.boundary.headers
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/check_server_header_fixture.py"
        --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}" --compiler "${CMAKE_CXX_COMPILER}"
        --source-dir "${PROJECT_SOURCE_DIR}/tests/boundary_fixture/header_leak"
        --build-dir "${CMAKE_CURRENT_BINARY_DIR}/header-fixture")
set_tests_properties(shark.boundary.headers PROPERTIES LABELS "cpu;tooling;boundary;server" TIMEOUT 60
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME shark.tooling.headless-environment
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_headless_environment.py" -v)
set_tests_properties(shark.tooling.headless-environment PROPERTIES LABELS "cpu;tooling;server" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-evidence
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_probe.py" -v)
set_tests_properties(shark.tooling.graphics-evidence PROPERTIES LABELS "cpu;tooling" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-dependencies
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_dependencies.py"
        --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}"
        --compiler "${CMAKE_CXX_COMPILER}" -v)
set_tests_properties(shark.tooling.graphics-dependencies PROPERTIES LABELS "cpu;tooling;boundary" TIMEOUT 90
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-tools
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_tools.py" -v)
set_tests_properties(shark.tooling.graphics-tools PROPERTIES LABELS "cpu;tooling" TIMEOUT 15
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-validation
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_validation.py" -v)
set_tests_properties(shark.tooling.graphics-validation PROPERTIES LABELS "cpu;tooling" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-package
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_package.py" -v)
set_tests_properties(shark.tooling.graphics-package PROPERTIES LABELS "cpu;tooling;boundary" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME shark.tooling.graphics-package-linux
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tests/test_graphics_package_linux.py" -v)
set_tests_properties(shark.tooling.graphics-package-linux PROPERTIES LABELS "cpu;tooling;boundary" TIMEOUT 30
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
