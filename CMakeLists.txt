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
    src/dt295.c src/dt295_crypto.c src/dt295_routes.c src/dt295_stream.c
    src/ts_output.c src/dt295_capture.c src/dt295_capture_host.c)
target_compile_definitions(pix_capture_c_backend PRIVATE _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
target_include_directories(pix_capture_c_backend PUBLIC include src)
target_link_libraries(pix_capture_c_backend PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_library(pix_capture_composition STATIC pix-capture-consumer.cpp)
target_compile_features(pix_capture_composition PUBLIC cxx_std_17)
target_include_directories(pix_capture_composition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE src)
target_link_libraries(pix_capture_composition PUBLIC dtv::capture pix_capture_c_backend)

add_executable(pix_capture_consumer_proof tests/dt295_consumer_proof.cpp)
target_include_directories(pix_capture_consumer_proof PRIVATE include src tests)
target_compile_definitions(pix_capture_consumer_proof PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(pix_capture_consumer_proof PRIVATE pix_capture_composition Threads::Threads)

add_executable(dt295_capture_sink_proof
    tests/dt295_capture_sink_proof.c tests/dt295_common_sink_adapter.cpp
    src/dt295_routes.c src/transport.c
    src/crypto.c src/platform.c)
target_include_directories(dt295_capture_sink_proof PRIVATE include src tests)
target_compile_definitions(dt295_capture_sink_proof PRIVATE _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
target_link_libraries(dt295_capture_sink_proof PRIVATE pix_capture_composition PkgConfig::LIBUSB Threads::Threads)
target_link_options(dt295_capture_sink_proof PRIVATE
    -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_cond_clockwait
    -Wl,--wrap=pthread_join -Wl,--wrap=pthread_create -Wl,--wrap=calloc
    -Wl,--wrap=dt295_capture_host_destroy)

add_executable(dt295_stream_native_proof tests/dt295_stream_native_proof.c src/dt295_stream.c)
target_include_directories(dt295_stream_native_proof PRIVATE include)
target_compile_definitions(dt295_stream_native_proof PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(dt295_stream_native_proof PRIVATE PkgConfig::LIBUSB Threads::Threads)
target_link_options(dt295_stream_native_proof PRIVATE
    -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_handle_events_timeout_completed
    -Wl,--wrap=libusb_cancel_transfer -Wl,--wrap=libusb_free_transfer
    -Wl,--wrap=pthread_join)

add_library(pix_capture_actual_pipeline_backend STATIC
    src/pid_filter.c src/control.c src/channel.c src/crypto.c src/platform.c
    src/protocol.c src/transport.c src/usb_selector.c src/usb_listing.c
    src/usb_libusb.c src/usb_log.c src/diagnostic.c
    src/dt295.c src/dt295_crypto.c src/dt295_routes.c src/dt295_stream.c
    src/ts_output.c src/dt295_capture.c src/dt295_capture_host.c)
target_compile_definitions(pix_capture_actual_pipeline_backend PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64 DT295_CAPTURE_TESTING)
target_include_directories(pix_capture_actual_pipeline_backend PUBLIC include src)
target_link_libraries(pix_capture_actual_pipeline_backend PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_library(pix_capture_actual_pipeline_composition STATIC pix-capture-consumer.cpp)
target_compile_features(pix_capture_actual_pipeline_composition PUBLIC cxx_std_17)
target_include_directories(pix_capture_actual_pipeline_composition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE src)
target_link_libraries(pix_capture_actual_pipeline_composition PUBLIC
    dtv::capture pix_capture_actual_pipeline_backend)

add_executable(dt295_actual_pipeline_proof
    tests/dt295_actual_fixture.c tests/dt295_actual_pipeline.cpp)
target_include_directories(dt295_actual_pipeline_proof PRIVATE include src tests)
target_compile_definitions(dt295_actual_pipeline_proof PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
target_link_libraries(dt295_actual_pipeline_proof PRIVATE
    pix_capture_actual_pipeline_composition Threads::Threads)
target_link_options(dt295_actual_pipeline_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer -Wl,--wrap=libusb_submit_transfer
    -Wl,--wrap=libusb_handle_events_timeout_completed
    -Wl,--wrap=libusb_cancel_transfer -Wl,--wrap=libusb_free_transfer
    -Wl,--wrap=dt295_capture_host_start
    -Wl,--wrap=dt295_capture_run_to_sink -Wl,--wrap=dt295_routes_commit
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=dt295_stream_stop
    -Wl,--wrap=dt295_stream_snapshot
    -Wl,--wrap=now_ms -Wl,--wrap=sleep_ms
    -Wl,--wrap=pthread_join -Wl,--wrap=pthread_cond_clockwait
    -Wl,--wrap=random_bytes)

set(PIX_CAPTURE_OLD_SOURCE old-control-source/pix-capture-consumer)
add_library(pix_capture_old_actual_backend STATIC
    ${PIX_CAPTURE_OLD_SOURCE}/src/pid_filter.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/control.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/channel.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/crypto.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/platform.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/protocol.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/transport.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/usb_selector.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/usb_listing.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/usb_libusb.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/usb_log.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/diagnostic.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/dt295.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/dt295_crypto.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/dt295_routes.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/dt295_stream.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/ts_output.c
    ${PIX_CAPTURE_OLD_SOURCE}/src/dt295_capture.c)
target_compile_definitions(pix_capture_old_actual_backend PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64 DT295_CAPTURE_TESTING)
target_include_directories(pix_capture_old_actual_backend PUBLIC
    ${PIX_CAPTURE_OLD_SOURCE}/include ${PIX_CAPTURE_OLD_SOURCE}/src)
target_link_libraries(pix_capture_old_actual_backend PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_executable(dt295_actual_old_pipeline
    tests/dt295_actual_fixture.c tests/dt295_actual_pipeline.cpp)
target_include_directories(dt295_actual_old_pipeline PRIVATE
    ${PIX_CAPTURE_OLD_SOURCE} ${PIX_CAPTURE_OLD_SOURCE}/include
    ${PIX_CAPTURE_OLD_SOURCE}/src tests)
target_compile_definitions(dt295_actual_old_pipeline PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64 DT295_ACTUAL_OLD_CONTROL)
target_link_libraries(dt295_actual_old_pipeline PRIVATE pix_capture_old_actual_backend Threads::Threads)
target_link_options(dt295_actual_old_pipeline PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer -Wl,--wrap=libusb_submit_transfer
    -Wl,--wrap=libusb_handle_events_timeout_completed
    -Wl,--wrap=libusb_cancel_transfer -Wl,--wrap=libusb_free_transfer
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=now_ms -Wl,--wrap=sleep_ms
    -Wl,--wrap=random_bytes -Wl,--wrap=ts_output_submit)
