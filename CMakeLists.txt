cmake_minimum_required(VERSION 3.16)
project(fsmvis CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

add_executable(tcpou2st src/tcpou2st/main.cpp)
add_executable(stvis    src/stvis/main.cpp)

install(TARGETS tcpou2st stvis DESTINATION bin)
