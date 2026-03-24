cmake_minimum_required(VERSION 3.18)
project(nsx_sim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(nsx_sim main.cpp)

target_compile_options(nsx_sim PRIVATE
    -O3 -Wall -Wextra -pthread
)

target_link_libraries(nsx_sim PRIVATE pthread)
