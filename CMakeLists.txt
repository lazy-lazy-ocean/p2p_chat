cmake_minimum_required(VERSION 3.20)
project(lan-chat VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

add_library(shared INTERFACE)
target_include_directories(shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shared)
target_link_libraries(shared INTERFACE nlohmann_json::nlohmann_json)

add_subdirectory(client)
