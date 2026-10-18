cmake_minimum_required(VERSION 3.16)
project(dtv_userland_subdirectory_consumer LANGUAGES CXX)

if(NOT DEFINED DTV_USERLAND_CORE_SOURCE)
    message(FATAL_ERROR "DTV_USERLAND_CORE_SOURCE must point to the common core source tree")
endif()

add_subdirectory("${DTV_USERLAND_CORE_SOURCE}" dtv_userland_core)
add_executable(subdirectory_consumer ../main.cpp)
target_compile_features(subdirectory_consumer PRIVATE cxx_std_17)
target_link_libraries(subdirectory_consumer PRIVATE dtv::core dtv::wire)
