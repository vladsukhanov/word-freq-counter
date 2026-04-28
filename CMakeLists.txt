cmake_minimum_required(VERSION 3.21)
project(freq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)

    add_compile_options(
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-march=native>"
        "$<$<CONFIG:Release>:-funroll-loops>"
        "$<$<CONFIG:Release>:-fomit-frame-pointer>"
    )

    add_compile_options("$<$<CONFIG:Debug>:-g3>")

    # MinGW doesn't support libasan
    if(NOT WIN32)
        add_compile_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
        add_link_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
    endif()
endif()

add_executable(freq src/freq.cpp)

install(TARGETS freq DESTINATION bin)