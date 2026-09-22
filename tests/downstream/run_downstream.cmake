# Installs the Change Planner package into a scratch prefix and proves that an
# independent downstream project can find, build against and run it.
#
# Invoked by CTest with:
#   CPLAN_SOURCE_DIR, CPLAN_BINARY_DIR, CPLAN_GENERATOR, CPLAN_CXX_COMPILER, CPLAN_CONFIG

set(prefix "${CPLAN_BINARY_DIR}/downstream-prefix")
set(consumer_build "${CPLAN_BINARY_DIR}/downstream-build")

message(STATUS "downstream: removing previous scratch directories")
file(REMOVE_RECURSE "${prefix}" "${consumer_build}")

message(STATUS "downstream: installing ChangePlanner into ${prefix}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${CPLAN_BINARY_DIR}" --prefix "${prefix}"
          --config "${CPLAN_CONFIG}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed (${install_result})\n${install_output}\n${install_error}")
endif()

if(NOT EXISTS "${prefix}/lib/cmake/ChangePlanner/ChangePlannerConfig.cmake")
  # Some generators place libraries under lib64 or a config-specific directory.
  file(GLOB config_candidates "${prefix}/*/cmake/ChangePlanner/ChangePlannerConfig.cmake"
       "${prefix}/lib/*/ChangePlannerConfig.cmake")
  if(NOT config_candidates)
    message(FATAL_ERROR "installed package config was not found under ${prefix}")
  endif()
endif()

if(NOT CPLAN_BUILD_TYPE)
  set(CPLAN_BUILD_TYPE "Release")
endif()
# A sanitizer-instrumented library must be linked into a sanitizer-instrumented
# consumer: the runtime flags travel with the artifact.
if(CPLAN_ENABLE_ASAN)
  set(CPLAN_CONSUMER_FLAGS "/fsanitize=address /Zi")
else()
  set(CPLAN_CONSUMER_FLAGS "")
endif()

message(STATUS "downstream: configuring the consumer project (${CPLAN_BUILD_TYPE})")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${CPLAN_SOURCE_DIR}/examples/downstream_consumer"
          -B "${consumer_build}"
          -G "${CPLAN_GENERATOR}"
          -DCMAKE_BUILD_TYPE=${CPLAN_BUILD_TYPE}
          -DCMAKE_CXX_COMPILER=${CPLAN_CXX_COMPILER}
          "-DCMAKE_CXX_FLAGS=${CPLAN_CONSUMER_FLAGS}"
          "-DCMAKE_EXE_LINKER_FLAGS=${CPLAN_CONSUMER_FLAGS}"
          -DCMAKE_PREFIX_PATH=${prefix}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "consumer configure failed\n${configure_output}\n${configure_error}")
endif()

message(STATUS "downstream: building the consumer project")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config ${CPLAN_BUILD_TYPE}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed\n${build_output}\n${build_error}")
endif()

message(STATUS "downstream: running the consumer")
file(GLOB consumer_binaries "${consumer_build}/downstream_consumer*")
if(NOT consumer_binaries)
  file(GLOB_RECURSE consumer_binaries "${consumer_build}/downstream_consumer*")
endif()
list(FILTER consumer_binaries EXCLUDE REGEX "CMakeFiles")
if(NOT consumer_binaries)
  message(FATAL_ERROR "consumer executable not found in ${consumer_build}")
endif()
list(GET consumer_binaries 0 consumer_binary)
execute_process(
  COMMAND "${consumer_binary}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
message(STATUS "downstream consumer output:\n${run_output}")
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer run failed (${run_result})\n${run_error}")
endif()

message(STATUS "downstream validation succeeded")
