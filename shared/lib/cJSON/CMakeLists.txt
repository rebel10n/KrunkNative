cmake_minimum_required(VERSION 3.10)

add_library(cJSON cJSON.c)
target_include_directories(cJSON PUBLIC .)