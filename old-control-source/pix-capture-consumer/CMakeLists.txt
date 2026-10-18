cmake_minimum_required(VERSION 3.16)
project(pix_capture_consumer LANGUAGES C CXX)

if(NOT DTV_USERLAND_CORE_SOURCE_DIR)
    message(FATAL_ERROR "Pass the explicit DTV_USERLAND_CORE_SOURCE_DIR source override")
endif()
set(DTV_USERLAND_BUILD_CAPTURE ON CACHE BOOL "Build packet capture storage" FORCE)
set(DTV_USERLAND_BUILD_RECEIVER OFF CACHE BOOL "Do not build receiver engine" FORCE)
add_subdirectory(${DTV_USERLAND_CORE_SOURCE_DIR} core-capture)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(pix_capture_c_backend STATIC
    src/pid_filter.c src/control.c src/channel.c src/crypto.c src/platform.c
    src/protocol.c src/transport.c src/usb_selector.c src/usb_listing.c
    src/usb_libusb.c src/usb_log.c src/diagnostic.c
    src/dt181_capture_bridge.c src/dt181_session.c src/dt181_capture_host.c)
target_compile_definitions(pix_capture_c_backend PRIVATE _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64 PIX_CAPTURE_BRIDGE_EXTERNAL)
target_include_directories(pix_capture_c_backend PUBLIC include src)
target_link_libraries(pix_capture_c_backend PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_library(pix_capture_composition STATIC pix-capture-consumer.cpp)
target_compile_features(pix_capture_composition PUBLIC cxx_std_17)
target_include_directories(pix_capture_composition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pix_capture_composition PUBLIC dtv::capture pix_capture_c_backend)

add_executable(pix_capture_consumer_proof
    tests/consumer_proof.cpp tests/pix_libusb_bottom.c)
target_include_directories(pix_capture_consumer_proof PRIVATE include src tests)
target_compile_definitions(pix_capture_consumer_proof PRIVATE _POSIX_C_SOURCE=200809L PIX_CAPTURE_BRIDGE_EXTERNAL)
target_link_libraries(pix_capture_consumer_proof PRIVATE pix_capture_composition Threads::Threads)
target_link_options(pix_capture_consumer_proof PRIVATE
    -Wl,--wrap=pixy_fifo_clear -Wl,--wrap=pixy_start -Wl,--wrap=pixy_stop
    -Wl,--wrap=pixy_certify -Wl,--wrap=pthread_create -Wl,--wrap=calloc)
target_link_options(pix_capture_consumer_proof PRIVATE -Wl,--wrap=pthread_join)
target_link_options(pix_capture_consumer_proof PRIVATE
    -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_cond_clockwait
    -Wl,--wrap=dt181_capture_host_destroy)
