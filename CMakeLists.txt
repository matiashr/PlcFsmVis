cmake_minimum_required(VERSION 3.16)
project(fsmvis CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -g)

add_executable(tcpou2st src/tcpou2st/main.cpp)
add_executable(stvis    src/stvis/main.cpp)

find_package(LibXml2 REQUIRED)
target_link_libraries(tcpou2st PRIVATE LibXml2::LibXml2)

install(TARGETS tcpou2st stvis DESTINATION bin)
