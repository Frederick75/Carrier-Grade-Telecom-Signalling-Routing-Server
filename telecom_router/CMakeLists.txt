cmake_minimum_required(VERSION 3.16)
project(telecom_router CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O3 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(routing_server src/routing_server.cpp)
target_link_libraries(routing_server rt pthread)

add_executable(flx_engine src/flx_engine.cpp)
target_link_libraries(flx_engine rt pthread)
