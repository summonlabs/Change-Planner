cmake_minimum_required(VERSION 3.24)

# Standalone downstream consumer of the installed Change Planner package. This
# project is intentionally NOT part of the Change Planner build tree: it consumes
# the exported target ChangePlanner::change_planner exactly as an external user
# would, after the parent project has been installed and exported.
project(ChangePlannerDownstreamConsumer
  VERSION 1.0.0
  DESCRIPTION "Minimal downstream consumer of the installed Change Planner package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ChangePlanner CONFIG REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE ChangePlanner::change_planner)
