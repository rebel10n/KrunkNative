cmake_minimum_required(VERSION 3.10)

add_library(glad src/glad.c)
target_include_directories(glad PUBLIC include)