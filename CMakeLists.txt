cmake_minimum_required(VERSION 3.16)
project(pc1500emu VERSION 0.7.5 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
