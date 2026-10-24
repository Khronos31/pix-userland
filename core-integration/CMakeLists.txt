cmake_minimum_required(VERSION 3.16)
project(pix_core_integration LANGUAGES C CXX)

include(FetchContent)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

set(PIX_CORE_SOURCE_DIR "" CACHE PATH "Local dtv-userland-core source override")
set(PIX_CORE_REVISION "5c392159f1fb3803f416a2907eb20ccc52f65254" CACHE STRING
    "dtv-userland-core revision")

set(DTV_USERLAND_BUILD_RECEIVER ON CACHE BOOL "Build receiver engine" FORCE)
set(DTV_USERLAND_BUILD_CAPTURE ON CACHE BOOL "Build packet capture session" FORCE)

if(PIX_CORE_SOURCE_DIR)
    add_subdirectory("${PIX_CORE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/dtv-userland-core")
else()
    FetchContent_Declare(dtv_userland_core
        GIT_REPOSITORY https://github.com/Khronos31/dtv-userland-core.git
        GIT_TAG "${PIX_CORE_REVISION}"
        GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(dtv_userland_core)
endif()

set(PIX_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

add_library(pix_dt181_c_backend STATIC
    "${PIX_SOURCE_DIR}/src/pid_filter.c"
    "${PIX_SOURCE_DIR}/src/control.c"
    "${PIX_SOURCE_DIR}/src/channel.c"
    "${PIX_SOURCE_DIR}/src/crypto.c"
    "${PIX_SOURCE_DIR}/src/platform.c"
    "${PIX_SOURCE_DIR}/src/protocol.c"
    "${PIX_SOURCE_DIR}/src/transport.c"
    "${PIX_SOURCE_DIR}/src/usb_selector.c"
    "${PIX_SOURCE_DIR}/src/usb_listing.c"
    "${PIX_SOURCE_DIR}/src/usb_libusb.c"
    "${PIX_SOURCE_DIR}/src/usb_log.c"
    "${PIX_SOURCE_DIR}/src/diagnostic.c"
    "${PIX_SOURCE_DIR}/src/dt181_capture_bridge.c"
    "${PIX_SOURCE_DIR}/src/dt181_session.c"
    "${PIX_SOURCE_DIR}/src/dt181_capture_host.c")
target_compile_definitions(pix_dt181_c_backend PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64 PIX_CAPTURE_BRIDGE_EXTERNAL)
target_include_directories(pix_dt181_c_backend PUBLIC
    "${PIX_SOURCE_DIR}/include" "${PIX_SOURCE_DIR}/src")
target_link_libraries(pix_dt181_c_backend PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_library(pix_dt181_composition STATIC
    "${PIX_SOURCE_DIR}/pix-dt181-consumer.cpp"
    "${PIX_SOURCE_DIR}/pix-dt181-receiver-adapter.cpp")
target_compile_features(pix_dt181_composition PUBLIC cxx_std_17)
target_include_directories(pix_dt181_composition PUBLIC "${PIX_SOURCE_DIR}")
target_link_libraries(pix_dt181_composition PUBLIC
    dtv::receiver dtv::capture pix_dt181_c_backend)
target_compile_options(pix_dt181_composition PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

enable_testing()
add_executable(pix_dt181_receiver_adapter_proof
    "${PIX_SOURCE_DIR}/tests/pix_dt181_receiver_adapter_proof.cpp"
    "${PIX_SOURCE_DIR}/tests/pix_libusb_bottom.c")
target_include_directories(pix_dt181_receiver_adapter_proof PRIVATE
    "${PIX_SOURCE_DIR}/tests")
target_compile_definitions(pix_dt181_receiver_adapter_proof PRIVATE
    _POSIX_C_SOURCE=200809L PIX_CAPTURE_BRIDGE_EXTERNAL)
target_compile_options(pix_dt181_receiver_adapter_proof PRIVATE
    -UNDEBUG -fno-exceptions -fno-rtti)
target_link_libraries(pix_dt181_receiver_adapter_proof PRIVATE
    pix_dt181_composition Threads::Threads)
target_link_options(pix_dt181_receiver_adapter_proof PRIVATE
    -Wl,--wrap=pixy_power -Wl,--wrap=pixy_session -Wl,--wrap=pixy_tuner_init
    -Wl,--wrap=pixy_tune_slot -Wl,--wrap=pixy_status
    -Wl,--wrap=pixy_fifo_clear -Wl,--wrap=pixy_start
    -Wl,--wrap=pixy_stop -Wl,--wrap=pixy_certify -Wl,--wrap=pixy_shutdown
    -Wl,--wrap=pthread_mutex_lock)
add_test(NAME pix_dt181_receiver_adapter_proof
    COMMAND pix_dt181_receiver_adapter_proof)

add_library(pix_dt295_c_backend STATIC
    "${PIX_SOURCE_DIR}/src/dt295.c"
    "${PIX_SOURCE_DIR}/src/dt295_crypto.c"
    "${PIX_SOURCE_DIR}/src/dt295_routes.c"
    "${PIX_SOURCE_DIR}/src/dt295_stream.c"
    "${PIX_SOURCE_DIR}/src/ts_output.c"
    "${PIX_SOURCE_DIR}/src/dt295_capture.c"
    "${PIX_SOURCE_DIR}/src/dt295_capture_host.c")
target_compile_definitions(pix_dt295_c_backend PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
target_include_directories(pix_dt295_c_backend PUBLIC
    "${PIX_SOURCE_DIR}/include" "${PIX_SOURCE_DIR}/src")
target_link_libraries(pix_dt295_c_backend PUBLIC
    pix_dt181_c_backend PkgConfig::LIBUSB Threads::Threads)

add_library(pix_dt295_composition STATIC
    "${PIX_SOURCE_DIR}/pix-dt295-receiver-adapter.cpp")
target_compile_features(pix_dt295_composition PUBLIC cxx_std_17)
target_include_directories(pix_dt295_composition PUBLIC "${PIX_SOURCE_DIR}")
target_link_libraries(pix_dt295_composition PUBLIC
    dtv::receiver dtv::capture pix_dt295_c_backend)
target_compile_options(pix_dt295_composition PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(pix_dt295_adapter_lifecycle_proof
    "${PIX_SOURCE_DIR}/tests/pix_dt295_adapter_lifecycle_proof.cpp"
    "${PIX_SOURCE_DIR}/tests/dt295_actual_fixture.c")
target_include_directories(pix_dt295_adapter_lifecycle_proof PRIVATE
    "${PIX_SOURCE_DIR}" "${PIX_SOURCE_DIR}/include"
    "${PIX_SOURCE_DIR}/src" "${PIX_SOURCE_DIR}/tests")
target_compile_definitions(pix_dt295_adapter_lifecycle_proof PRIVATE
    _POSIX_C_SOURCE=200809L)
target_compile_features(pix_dt295_adapter_lifecycle_proof PRIVATE cxx_std_17)
target_compile_options(pix_dt295_adapter_lifecycle_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pix_dt295_adapter_lifecycle_proof PRIVATE
    pix_dt295_composition Threads::Threads)
target_link_options(pix_dt295_adapter_lifecycle_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=dt295_stream_stop -Wl,--wrap=dt295_stream_destroy
    -Wl,--wrap=random_bytes)
add_test(NAME pix_dt295_adapter_lifecycle_proof
    COMMAND pix_dt295_adapter_lifecycle_proof)
