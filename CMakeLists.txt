cmake_minimum_required(VERSION 3.17)
project(myfat C)

set(CMAKE_C_STANDARD 99)
add_compile_options(-D_FILE_OFFSET_BITS=64)

add_executable(myfat my_fat.c main.c)
add_executable(test my_fat.c test.c)

target_link_libraries(test -lfuse3)
target_link_libraries(myfat -lfuse3)