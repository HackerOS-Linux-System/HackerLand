cmake_minimum_required(VERSION 3.16)
project(hackerland)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MIRAL miral REQUIRED IMPORTED_TARGET)
find_package(Threads REQUIRED)

add_executable(hackerland
    src/main.cpp
    src/tiling_window_manager.cpp
    src/config.cpp
    src/ipc_server.cpp
)

target_link_libraries(hackerland
    PkgConfig::MIRAL
    Threads::Threads
    stdc++fs
)
