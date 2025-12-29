cmake_minimum_required(VERSION 3.10)

project(p2p VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_executable(client src/client.cpp)
add_executable(server src/server.cpp)

target_include_directories(client PUBLIC include)
target_include_directories(server PUBLIC include)
