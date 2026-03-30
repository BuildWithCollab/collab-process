cmake_minimum_required(VERSION 3.20)
project(collab-process-smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(collab-process CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)

add_executable(smoke-test tests.cpp)
target_link_libraries(smoke-test PRIVATE collab::collab-process Catch2::Catch2WithMain)
