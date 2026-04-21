cmake_minimum_required(VERSION 3.10)

add_library(pcg_basic pcg_basic.c)
target_include_directories(pcg_basic PUBLIC .)