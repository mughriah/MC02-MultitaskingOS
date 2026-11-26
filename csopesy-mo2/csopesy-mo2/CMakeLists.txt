cmake_minimum_required(VERSION 3.10)

project(csopesy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(csopesy
    main.cpp
    console.cpp
    config.cpp
    scheduler.cpp
    process.cpp
    instruction.cpp
    memory_manager.cpp
    report.cpp
)

target_link_libraries(csopesy Threads::Threads)

# Set Visual Studio working directory to the project folder
set_property(TARGET csopesy PROPERTY
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Copy config.txt to the build directory
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/config.txt"
    COPYONLY
)