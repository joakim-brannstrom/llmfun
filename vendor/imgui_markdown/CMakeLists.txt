cmake_minimum_required (VERSION 3.10)
project(imgui_markdown VERSION 1.0.0)

add_library(imgui_markdown INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/imgui_markdown.h)
target_include_directories(imgui_markdown INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

