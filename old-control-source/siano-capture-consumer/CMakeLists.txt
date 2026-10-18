cmake_minimum_required(VERSION 3.16)
project(siano_capture_consumer LANGUAGES C CXX)

if(NOT DTV_USERLAND_CORE_SOURCE_DIR)
    message(FATAL_ERROR "Pass the explicit DTV_USERLAND_CORE_SOURCE_DIR source override")
endif()
set(DTV_USERLAND_BUILD_CAPTURE ON CACHE BOOL "Build packet capture storage" FORCE)
set(DTV_USERLAND_BUILD_RECEIVER OFF CACHE BOOL "Do not build receiver engine" FORCE)
add_subdirectory(${DTV_USERLAND_CORE_SOURCE_DIR} core-capture)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(siano_capture_c_backend STATIC
    siano-device.c siano-capture-host.c protocol.c stream-state.c control-parse.c control-input.c
    device-selector.c exit-codes.c queue-policy.c output-writer.c)
target_compile_features(siano_capture_c_backend PUBLIC c_std_11)
target_compile_definitions(siano_capture_c_backend PRIVATE _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
target_link_libraries(siano_capture_c_backend PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_library(siano_capture_composition STATIC siano-capture-consumer.cpp)
target_compile_features(siano_capture_composition PUBLIC cxx_std_17)
target_compile_definitions(siano_capture_composition PRIVATE SIANO_CAPTURE_TEST_SEAMS)
target_include_directories(siano_capture_composition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(siano_capture_composition PUBLIC dtv::capture siano_capture_c_backend)

add_executable(siano_capture_consumer_proof
    tests/consumer_proof.cpp
    tests/libusb-loopback.c)
target_compile_features(siano_capture_consumer_proof PRIVATE cxx_std_17)
target_compile_definitions(siano_capture_consumer_proof PRIVATE SIANO_CAPTURE_TEST_SEAMS)
target_link_libraries(siano_capture_consumer_proof PRIVATE siano_capture_composition)
target_link_options(siano_capture_consumer_proof PRIVATE
    -Wl,--wrap=pthread_join -Wl,--wrap=pthread_create
    -Wl,--wrap=pthread_cond_timedwait -Wl,--wrap=pthread_cond_clockwait
    -Wl,--wrap=calloc)
