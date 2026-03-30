cmake_minimum_required(VERSION 3.20)
project(collab-process LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fmt CONFIG REQUIRED)
find_package(collab-core CONFIG REQUIRED)

add_library(collab-process STATIC
    src/env_block.cpp
    src/find_executable.cpp
    src/process_ref.cpp
    src/result.cpp
    src/run.cpp
    src/running_process.cpp
    src/write_temp_file.cpp
)

if(WIN32)
    target_sources(collab-process PRIVATE
        src/win32/console.cpp
        src/win32/pe_check.cpp
        src/win32/spawn.cpp
    )
else()
    target_sources(collab-process PRIVATE
        src/unix/pe_check.cpp
        src/unix/spawn.cpp
    )
endif()

target_include_directories(collab-process PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(collab-process PRIVATE src)
target_link_libraries(collab-process PUBLIC fmt::fmt collab::collab-core)

install(TARGETS collab-process EXPORT collab-process-targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT collab-process-targets
    FILE collab-process-config.cmake
    NAMESPACE collab::
    DESTINATION share/collab-process
)
