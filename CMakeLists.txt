cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED true)

project(objLoader)

find_library(URING uring REQUIRED)

file(GLOB SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cpp)

add_executable(obj_loader ${SOURCES})

target_link_libraries(obj_loader ${URING})