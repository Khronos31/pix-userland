cmake_minimum_required(VERSION 3.16)
project(pix_capture_consumer LANGUAGES C CXX)

include(FetchContent)

set(DTV_USERLAND_CORE_SOURCE_DIR "" CACHE PATH
    "Explicit local dtv-userland-core source override")
set(PX4_USERLAND_SOURCE_DIR "" CACHE PATH
    "Explicit local PX4 userland source override")
set(PIX_RUNTIME_VERSION "0.0.0-dev" CACHE STRING
    "PIX runtime version (default is an unreleased development build)")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/dtv-userland-core.revision" DTV_CORE_REVISION)
string(STRIP "${DTV_CORE_REVISION}" DTV_CORE_REVISION)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/px4-userland.revision" PX4_USERLAND_REVISION)
string(STRIP "${PX4_USERLAND_REVISION}" PX4_USERLAND_REVISION)
foreach(_revision IN ITEMS DTV_CORE_REVISION PX4_USERLAND_REVISION)
    string(LENGTH "${${_revision}}" _revision_length)
    if(NOT _revision_length EQUAL 40 OR NOT "${${_revision}}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${_revision} must contain a full 40-hex commit")
    endif()
endforeach()

if(DTV_USERLAND_CORE_SOURCE_DIR)
    set(DTV_CORE_SOURCE "${DTV_USERLAND_CORE_SOURCE_DIR}")
    message(WARNING "dtv-userland-core: using explicit local source ${DTV_CORE_SOURCE}; build is not pinned")
else()
    FetchContent_Declare(dtv_userland_core
        GIT_REPOSITORY "git@github.com:Khronos31/dtv-userland-core.git"
        GIT_TAG "${DTV_CORE_REVISION}")
    FetchContent_GetProperties(dtv_userland_core)
    if(NOT dtv_userland_core_POPULATED)
        FetchContent_Populate(dtv_userland_core)
    endif()
    set(DTV_CORE_SOURCE "${dtv_userland_core_SOURCE_DIR}")
endif()
if(PX4_USERLAND_SOURCE_DIR)
    set(PX4_SOURCE "${PX4_USERLAND_SOURCE_DIR}")
    message(WARNING "PX4 userland: using explicit local source ${PX4_SOURCE}; build is not pinned")
else()
    FetchContent_Declare(px4_userland
        GIT_REPOSITORY "git@github.com:Khronos31/px4-userland.git"
        GIT_TAG "${PX4_USERLAND_REVISION}")
    FetchContent_GetProperties(px4_userland)
    if(NOT px4_userland_POPULATED)
        FetchContent_Populate(px4_userland)
    endif()
    set(PX4_SOURCE "${px4_userland_SOURCE_DIR}")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/dependency-sources.txt"
    "dtv-userland-core=${DTV_CORE_SOURCE}\nexpected=${DTV_CORE_REVISION}\nPX4 userland=${PX4_SOURCE}\nexpected=${PX4_USERLAND_REVISION}\n")
set(PX4_ROOT "${PX4_SOURCE}/userland")
set(PIX_RUNTIME_VERSION "${PIX_RUNTIME_VERSION}" CACHE STRING "PIX runtime version" FORCE)
set(DTV_USERLAND_BUILD_CARD ON CACHE BOOL "Build card ownership engine" FORCE)
set(DTV_USERLAND_BUILD_CAPTURE ON CACHE BOOL "Build packet capture storage" FORCE)
set(DTV_USERLAND_BUILD_RECEIVER ON CACHE BOOL "Build receiver engine" FORCE)
set(DTV_USERLAND_BUILD_HOST_CTL ON CACHE BOOL "Build shared host control frontend" FORCE)
set(DTV_USERLAND_HOST_CTL_FACADE_INCLUDE_DIRS "${PX4_ROOT}/include" CACHE STRING
    "Product facade include directories for the shared host control frontend" FORCE)
if(NOT EXISTS "${DTV_CORE_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "dtv-userland-core source has no CMakeLists.txt: ${DTV_CORE_SOURCE}")
endif()
add_subdirectory(${DTV_CORE_SOURCE} core-capture)
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

add_executable(pix_dt295_adapter_lifecycle_proof
    tests/pix_dt295_adapter_lifecycle_proof.cpp tests/dt295_actual_fixture.c)
target_include_directories(pix_dt295_adapter_lifecycle_proof PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} include src tests)
target_compile_definitions(pix_dt295_adapter_lifecycle_proof PRIVATE
    _POSIX_C_SOURCE=200809L)
target_compile_features(pix_dt295_adapter_lifecycle_proof PRIVATE cxx_std_17)
set_source_files_properties(tests/pix_dt295_adapter_lifecycle_proof.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Werror;-fno-exceptions;-fno-rtti")
target_compile_options(pix_dt295_adapter_lifecycle_proof PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-UNDEBUG>")
target_link_libraries(pix_dt295_adapter_lifecycle_proof PRIVATE
    pix_dt181_runtime Threads::Threads)
target_link_options(pix_dt295_adapter_lifecycle_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=dt295_stream_stop -Wl,--wrap=dt295_stream_destroy
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

add_library(pix_dt181_c_backend STATIC
    src/pid_filter.c src/control.c src/channel.c src/crypto.c src/platform.c
    src/protocol.c src/transport.c src/usb_selector.c src/usb_listing.c
    src/usb_libusb.c src/usb_log.c src/diagnostic.c
    src/dt181_capture_bridge.c src/dt181_session.c src/dt181_capture_host.c
    src/dt295.c src/dt295_crypto.c src/dt295_routes.c src/dt295_stream.c)
target_compile_definitions(pix_dt181_c_backend PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64 PIX_CAPTURE_BRIDGE_EXTERNAL)
target_include_directories(pix_dt181_c_backend PUBLIC include src)
target_link_libraries(pix_dt181_c_backend PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_library(pix_dt181_composition STATIC
    pix-dt181-consumer.cpp pix-dt181-receiver-adapter.cpp)
target_compile_features(pix_dt181_composition PUBLIC cxx_std_17)
target_include_directories(pix_dt181_composition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pix_dt181_composition PUBLIC
    dtv::receiver dtv::capture pix_dt181_c_backend)
target_compile_options(pix_dt181_composition PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(pix_dt181_receiver_adapter_proof
    tests/pix_dt181_receiver_adapter_proof.cpp tests/pix_libusb_bottom.c)
target_include_directories(pix_dt181_receiver_adapter_proof PRIVATE
    include src tests)
target_compile_definitions(pix_dt181_receiver_adapter_proof PRIVATE
    _POSIX_C_SOURCE=200809L PIX_CAPTURE_BRIDGE_EXTERNAL)
target_compile_options(pix_dt181_receiver_adapter_proof PRIVATE
    -fno-exceptions -fno-rtti)
target_link_libraries(pix_dt181_receiver_adapter_proof PRIVATE
    pix_dt181_composition Threads::Threads)
target_link_options(pix_dt181_receiver_adapter_proof PRIVATE
    -Wl,--wrap=pixy_power -Wl,--wrap=pixy_session -Wl,--wrap=pixy_tuner_init
    -Wl,--wrap=pixy_tune_slot -Wl,--wrap=pixy_status
    -Wl,--wrap=pixy_fifo_clear -Wl,--wrap=pixy_start
    -Wl,--wrap=pixy_stop -Wl,--wrap=pixy_certify -Wl,--wrap=pixy_shutdown
    -Wl,--wrap=pthread_mutex_lock)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(PX4_RUNTIME_SOURCES
    ${PX4_ROOT}/src/ipc.cpp
    ${PX4_ROOT}/src/posix_ipc.cpp
    ${PX4_ROOT}/src/posix_tuner_nonce.cpp
    ${PX4_ROOT}/src/control_server.cpp
    ${PX4_ROOT}/src/control_workers.cpp
    ${PX4_ROOT}/src/tuner_service.cpp
    ${PX4_ROOT}/src/card_service.cpp
    ${PX4_ROOT}/src/enclosure_context.cpp
    ${PX4_ROOT}/src/control_client.cpp
    ${PX4_ROOT}/src/identity.cpp
    ${PX4_ROOT}/tools/px4_ts_core.cpp
    ${PX4_ROOT}/tools/px4_ts_posix.cpp
    ${PX4_ROOT}/tools/px4d_signals.cpp)
add_library(pix_px4_runtime STATIC ${PX4_RUNTIME_SOURCES})
target_include_directories(pix_px4_runtime PUBLIC
    ${PX4_ROOT}/include ${PX4_ROOT}/src ${PX4_ROOT}/tools)
target_compile_definitions(pix_px4_runtime PRIVATE
    PX4_USERLAND_RUNTIME_PRODUCT="pix-userland")
target_compile_features(pix_px4_runtime PUBLIC cxx_std_17)
# Shared core's CARD request enum is additive. This legacy host-runtime
# target does not advertise CARD_SERVICE: unknown CARD requests are refused
# by the negotiated core feature gate, before the legacy dispatch switch.
# Do not suppress other -Werror diagnostics or alter normal tuner behavior.
target_compile_options(pix_px4_runtime PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wno-error=switch -fno-exceptions -fno-rtti)
target_link_libraries(pix_px4_runtime PUBLIC Threads::Threads
    dtv::core dtv::wire dtv::control dtv::identity dtv::tuning
    dtv::receiver dtv::capture dtv::card)

add_library(pix_dt181_runtime STATIC pix_runtime.cpp pix-dt295-receiver-adapter.cpp)
target_include_directories(pix_dt181_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PX4_ROOT}/include ${PX4_ROOT}/src
    ${PX4_ROOT}/tools PRIVATE include src)
target_compile_definitions(pix_dt181_runtime PRIVATE
    _POSIX_C_SOURCE=200809L PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}")
target_compile_features(pix_dt181_runtime PUBLIC cxx_std_17)
target_compile_options(pix_dt181_runtime PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pix_dt181_runtime PUBLIC
    pix_px4_runtime pix_dt181_composition dtv::core dtv::control
    dtv::identity dtv::tuning PkgConfig::LIBUSB Threads::Threads)

add_executable(pixd pixd.cpp)
target_compile_definitions(pixd PRIVATE PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}")
target_compile_features(pixd PRIVATE cxx_std_17)
target_link_libraries(pixd PRIVATE pix_dt181_runtime dtv::json PkgConfig::LIBUSB)
target_compile_options(pixd PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(pixctl tools/pixctl.cpp)
target_include_directories(pixctl PRIVATE ${PX4_ROOT}/tools)
target_compile_definitions(pixctl PRIVATE
    PX4CTL_PRODUCT_VERSION="${PIX_RUNTIME_VERSION}")
target_compile_features(pixctl PRIVATE cxx_std_17)
target_link_libraries(pixctl PRIVATE dtv::host_ctl pix_px4_runtime dtv::json dtv::identity)
target_compile_options(pixctl PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(pix-ts userland/tools/pix_ts.cpp
    ${PX4_ROOT}/tools/px4_ts_core.cpp ${PX4_ROOT}/tools/px4_ts_posix.cpp)
target_include_directories(pix-ts PRIVATE ${PX4_ROOT}/include ${PX4_ROOT}/tools)
target_compile_definitions(pix-ts PRIVATE PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}")
target_compile_features(pix-ts PRIVATE cxx_std_17)
target_link_libraries(pix-ts PRIVATE pix_px4_runtime dtv::tuning)
target_compile_options(pix-ts PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(pix_runtime_list_proof tests/pix_runtime_list_proof.cpp)
target_link_libraries(pix_runtime_list_proof PRIVATE pix_dt181_runtime)
target_compile_features(pix_runtime_list_proof PRIVATE cxx_std_17)
target_compile_options(pix_runtime_list_proof PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)

add_executable(pixd_fixture pixd.cpp tests/pix-public-runtime-bottom.c
    tests/pix-public-runtime-libusb-bottom.c)
target_include_directories(pixd_fixture PRIVATE tests include src
    ${PX4_ROOT}/include ${PX4_ROOT}/src ${PX4_ROOT}/tools)
target_compile_definitions(pixd_fixture PRIVATE _POSIX_C_SOURCE=200809L
    PIX_CAPTURE_BRIDGE_EXTERNAL PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}"
    PIX_RUNTIME_TEST_CLOSE_BUDGET_MS=50 PIX_TEST_CALLBACK_GATE)
target_compile_features(pixd_fixture PRIVATE cxx_std_17)
target_link_libraries(pixd_fixture PRIVATE pix_dt181_runtime dtv::json)
target_link_options(pixd_fixture PRIVATE
    -Wl,--wrap=pixy_power -Wl,--wrap=pixy_session -Wl,--wrap=pixy_tuner_init
    -Wl,--wrap=pixy_tune_slot -Wl,--wrap=pixy_status
    -Wl,--wrap=pixy_fifo_clear -Wl,--wrap=pixy_start
    -Wl,--wrap=pixy_stop -Wl,--wrap=pixy_certify -Wl,--wrap=pixy_shutdown
    -Wl,--wrap=libusb_release_interface -Wl,--wrap=libusb_close
    -Wl,--wrap=pthread_create -Wl,--wrap=libusb_handle_events_timeout)

add_executable(pix_public_runtime_proof tests/pix_public_runtime_proof.cpp
    tests/pix-public-runtime-bottom.c tests/pix-public-runtime-libusb-bottom.c)
target_include_directories(pix_public_runtime_proof PRIVATE tests
    ${PX4_ROOT}/include ${PX4_ROOT}/tools include src)
target_compile_definitions(pix_public_runtime_proof PRIVATE _POSIX_C_SOURCE=200809L
    PIXD_PATH="$<TARGET_FILE:pixd_fixture>" PIXTS_PATH="$<TARGET_FILE:pix-ts>"
    PIXCTL_PATH="$<TARGET_FILE:pixctl>")
target_compile_features(pix_public_runtime_proof PRIVATE cxx_std_17)
target_link_libraries(pix_public_runtime_proof PRIVATE pix_dt181_runtime Threads::Threads)
target_link_options(pix_public_runtime_proof PRIVATE
    -Wl,--wrap=pixy_power -Wl,--wrap=pixy_session -Wl,--wrap=pixy_tuner_init
    -Wl,--wrap=pixy_tune_slot -Wl,--wrap=pixy_status
    -Wl,--wrap=pixy_fifo_clear -Wl,--wrap=pixy_start
    -Wl,--wrap=pixy_stop -Wl,--wrap=pixy_certify -Wl,--wrap=pixy_shutdown
    -Wl,--wrap=libusb_release_interface -Wl,--wrap=libusb_close
    -Wl,--wrap=pthread_create)
add_dependencies(pix_public_runtime_proof pixd_fixture pix-ts pixctl)
