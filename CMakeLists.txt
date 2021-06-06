cmake_minimum_required(VERSION 3.10)
project(myfat C)

add_definitions(-Wall)
set(CMAKE_C_STANDARD 99)
add_compile_options(-D_FILE_OFFSET_BITS=64)

add_executable(myfat my_fat.c main.c)

target_link_libraries(myfat -lfuse3)
