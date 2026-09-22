cmake_minimum_required(VERSION 3.24)

project(ChangePlanner
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral change planning runtime for Summon Software Labs Fabric OS"
  HOMEPAGE_URL "https://github.com/summonlabs/Change-Planner"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CPLAN_BUILD_TESTS "Build the Change Planner test suites" ON)
option(CPLAN_BUILD_BENCHMARKS "Build the Change Planner benchmarks" ON)
option(CPLAN_BUILD_EXAMPLES "Build the Change Planner examples" ON)
option(CPLAN_ENABLE_ASAN "Build with AddressSanitizer runtime checking" OFF)
option(CPLAN_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# First-party warning and hardening policy.
#
# The flags are applied per target instead of through an INTERFACE library so
# that nothing the package exports carries the build policy of this repository.
# ---------------------------------------------------------------------------
function(cplan_apply_flags target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:throwingNew /utf-8 /EHsc
      /external:anglebrackets /external:W0)
    if(CPLAN_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
    if(CPLAN_ENABLE_ASAN)
      target_compile_options(${target_name} PRIVATE /fsanitize=address /Zi /Od)
      target_link_options(${target_name} PRIVATE /fsanitize=address /INCREMENTAL:NO)
    endif()
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(CPLAN_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
    if(CPLAN_ENABLE_ASAN)
      target_compile_options(${target_name} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target_name} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(CPLAN_CORE_SOURCES
  src/core/status.cpp
  src/core/strong.cpp
  src/core/utf8.cpp
  src/core/digest.cpp
  src/core/canonical.cpp
  src/core/json.cpp
  src/model/state.cpp
  src/model/evidence.cpp
  src/model/target.cpp
  src/model/constraints.cpp
  src/model/operation.cpp
  src/model/evaluate.cpp
  src/safety/checker.cpp
  src/plan/plan.cpp
  src/plan/objective.cpp
  src/plan/refusal.cpp
  src/plan/graph.cpp
  src/plan/generate.cpp
  src/plan/verify.cpp
  src/plan/invalidate.cpp
  src/plan/replan.cpp
  src/plan/compare.cpp
  src/persist/artifact.cpp
  src/persist/repository.cpp
  src/cli/io.cpp
  src/cli/commands.cpp)

add_library(change_planner ${CPLAN_CORE_SOURCES})
add_library(ChangePlanner::change_planner ALIAS change_planner)
target_include_directories(change_planner
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
cplan_apply_flags(change_planner)
set_target_properties(change_planner PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# CLI executable
# ---------------------------------------------------------------------------
add_executable(cplan_cli src/cli/main.cpp)
set_target_properties(cplan_cli PROPERTIES OUTPUT_NAME cplan)
target_link_libraries(cplan_cli PRIVATE change_planner)
cplan_apply_flags(cplan_cli)

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS change_planner cplan_cli
  EXPORT ChangePlannerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/change_planner DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ChangePlannerTargets
  FILE ChangePlannerTargets.cmake
  NAMESPACE ChangePlanner::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChangePlanner)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ChangePlannerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ChangePlannerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChangePlanner)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ChangePlannerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ChangePlannerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ChangePlannerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChangePlanner)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CPLAN_BUILD_TESTS)
  enable_testing()

  add_library(cplan_test_support
    tests/support/test_framework.cpp
    tests/support/fixtures.cpp)
  target_include_directories(cplan_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(cplan_test_support PUBLIC change_planner)
  cplan_apply_flags(cplan_test_support)

  function(cplan_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE cplan_test_support)
    target_compile_definitions(${name} PRIVATE
      CPLAN_EXAMPLE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/data")
    cplan_apply_flags(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endfunction()

  cplan_add_test(cplan_core_tests tests/unit/core_test.cpp)
  cplan_add_test(cplan_model_tests tests/unit/model_test.cpp)
  cplan_add_test(cplan_safety_tests tests/unit/safety_test.cpp)
  cplan_add_test(cplan_plan_tests tests/unit/plan_test.cpp)
  cplan_add_test(cplan_persist_tests tests/unit/persist_test.cpp)
  cplan_add_test(cplan_property_tests tests/property/planner_property_test.cpp)
  cplan_add_test(cplan_adversarial_tests tests/adversarial/adversarial_test.cpp)
  cplan_add_test(cplan_concurrency_tests tests/concurrency/concurrency_test.cpp)

  # Independent-process tests: the CLI and the process helper are exercised as
  # real OS processes, including a real process kill during publication.
  add_executable(cplan_process_helper tests/tools/process_helper.cpp)
  target_link_libraries(cplan_process_helper PRIVATE change_planner)
  cplan_apply_flags(cplan_process_helper)

  add_executable(cplan_process_tests tests/process/process_test.cpp)
  target_link_libraries(cplan_process_tests PRIVATE cplan_test_support)
  target_compile_definitions(cplan_process_tests PRIVATE
    CPLAN_EXAMPLE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/data")
  cplan_apply_flags(cplan_process_tests)
  add_test(NAME cplan_process_tests
    COMMAND cplan_process_tests $<TARGET_FILE:cplan_cli> $<TARGET_FILE:cplan_process_helper>)
  set_tests_properties(cplan_process_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  # Downstream consumer: install the package into a scratch prefix, then build
  # and run an independent project against the installed artifact.
  add_test(NAME cplan_downstream_consumer
    COMMAND ${CMAKE_COMMAND}
      -DCPLAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DCPLAN_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DCPLAN_GENERATOR=${CMAKE_GENERATOR}
      -DCPLAN_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DCPLAN_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DCPLAN_ENABLE_ASAN=${CPLAN_ENABLE_ASAN}
      -DCPLAN_CONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/downstream/run_downstream.cmake)
  set_tests_properties(cplan_downstream_consumer PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(CPLAN_BUILD_BENCHMARKS)
  add_executable(cplan_bench benchmarks/bench_main.cpp)
  target_link_libraries(cplan_bench PRIVATE change_planner)
  cplan_apply_flags(cplan_bench)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(CPLAN_BUILD_EXAMPLES)
  add_executable(cplan_example_basic examples/basic_plan.cpp)
  target_link_libraries(cplan_example_basic PRIVATE change_planner)
  cplan_apply_flags(cplan_example_basic)
endif()

