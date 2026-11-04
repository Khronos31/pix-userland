cmake_minimum_required(VERSION 3.16)
project(pix_core_integration LANGUAGES C CXX)

include(FetchContent)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/dtv-userland-core.revision" PIX_CORE_PIN LIMIT_COUNT 1)
set(PIX_CORE_SOURCE_DIR "" CACHE PATH "Local dtv-userland-core source override")
set(PIX_CORE_REVISION "${PIX_CORE_PIN}" CACHE STRING "dtv-userland-core revision")
set(PIX_CORE_BUNDLED_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/dtv-userland-core")
if(NOT PIX_CORE_SOURCE_DIR AND EXISTS "${PIX_CORE_BUNDLED_DIR}/CMakeLists.txt")
    set(PIX_CORE_SOURCE_DIR "${PIX_CORE_BUNDLED_DIR}")
endif()

set(DTV_USERLAND_BUILD_RECEIVER ON CACHE BOOL "Build receiver engine" FORCE)
set(DTV_USERLAND_BUILD_CARD ON CACHE BOOL "Build card ownership engine" FORCE)
set(DTV_USERLAND_BUILD_CAPTURE ON CACHE BOOL "Build packet capture session" FORCE)
set(DTV_USERLAND_BUILD_CONTROL ON CACHE BOOL "Build control protocol gate" FORCE)
set(DTV_USERLAND_BUILD_STATUS_PROJECTION ON CACHE BOOL "Build receiver STATUS projection" FORCE)
set(DTV_USERLAND_BUILD_AUTHORITY ON CACHE BOOL "Build control authority" FORCE)
set(DTV_USERLAND_BUILD_HOST_TUNER ON CACHE BOOL "Build host tuner owner" FORCE)
set(DTV_USERLAND_BUILD_HOST_WORKERS ON CACHE BOOL "Build host control workers" FORCE)
set(DTV_USERLAND_BUILD_HOST_AUTHORITY_OWNER ON CACHE BOOL "Build host authority owner" FORCE)
set(DTV_USERLAND_BUILD_HOST_AUTHORITY_BRIDGE ON CACHE BOOL "Build host authority bridge" FORCE)
set(DTV_USERLAND_BUILD_HOST_RUNTIME ON CACHE BOOL "Build host runtime" FORCE)
set(DTV_USERLAND_BUILD_HOST_LISTENER_CONTEXT ON CACHE BOOL "Build listener Context" FORCE)
set(DTV_USERLAND_BUILD_HOST_FACADE ON CACHE BOOL "Build host process facade" FORCE)
set(DTV_USERLAND_BUILD_HOST_CTL ON CACHE BOOL "Build shared host control client" FORCE)
set(DTV_USERLAND_BUILD_HOST_TS ON CACHE BOOL "Build shared host TS client" FORCE)

if(PIX_CORE_SOURCE_DIR)
    add_subdirectory("${PIX_CORE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/dtv-userland-core")
    set(PIX_CORE_VERSION_SOURCE_DIR "${PIX_CORE_SOURCE_DIR}")
else()
    FetchContent_Declare(dtv_userland_core
        GIT_REPOSITORY https://github.com/Khronos31/dtv-userland-core.git
        GIT_TAG "${PIX_CORE_REVISION}"
        GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(dtv_userland_core)
    set(PIX_CORE_VERSION_SOURCE_DIR "${dtv_userland_core_SOURCE_DIR}")
endif()
file(STRINGS "${PIX_CORE_VERSION_SOURCE_DIR}/CMakeLists.txt" DTV_CORE_PROJECT_LINE
    REGEX "^project\\(dtv_userland_core VERSION ")
if(NOT DTV_CORE_PROJECT_LINE MATCHES
        "^project\\(dtv_userland_core VERSION ([0-9]+\\.[0-9]+\\.[0-9]+) LANGUAGES CXX\\)$")
    message(FATAL_ERROR "Could not read dtv-userland-core project version")
endif()
set(DTV_USERLAND_CORE_VERSION "${CMAKE_MATCH_1}")

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
# These proofs inject pthread mutexes and POSIX processes; Windows uses the
# same public runtime USB fixture with its native process harness below.
if(NOT WIN32)
add_executable(pix_dt181_receiver_adapter_proof
    "${PIX_SOURCE_DIR}/tests/pix_dt181_receiver_adapter_proof.cpp"
    "${PIX_SOURCE_DIR}/tests/pix_libusb_bottom.c")
target_include_directories(pix_dt181_receiver_adapter_proof PRIVATE
    "${PIX_SOURCE_DIR}/tests")
target_compile_definitions(pix_dt181_receiver_adapter_proof PRIVATE
    _POSIX_C_SOURCE=200809L PIX_CAPTURE_BRIDGE_EXTERNAL)
target_compile_options(pix_dt181_receiver_adapter_proof PRIVATE
    -UNDEBUG "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
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
endif()

add_library(pix_dt295_c_backend STATIC
    "${PIX_SOURCE_DIR}/src/dt295.c"
    "${PIX_SOURCE_DIR}/src/dt295_crypto.c"
    "${PIX_SOURCE_DIR}/src/dt295_routes.c"
    "${PIX_SOURCE_DIR}/src/dt295_stream.c"
    "${PIX_SOURCE_DIR}/src/ts_output.c"
    "${PIX_SOURCE_DIR}/src/dt295_capture.c"
    "${PIX_SOURCE_DIR}/src/dt295_live_policy.c")
target_compile_definitions(pix_dt295_c_backend PRIVATE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
if(WIN32)
    target_compile_definitions(pix_dt295_c_backend PRIVATE DT295_STREAM_WINDOWS_ENABLE)
endif()
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

if(NOT WIN32)
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
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
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

add_executable(pix_dt295_stream_retirement_proof
    "${PIX_SOURCE_DIR}/tests/pix_dt295_stream_retirement_proof.cpp"
    "${PIX_SOURCE_DIR}/tests/dt295_actual_fixture.c")
target_include_directories(pix_dt295_stream_retirement_proof PRIVATE
    "${PIX_SOURCE_DIR}" "${PIX_SOURCE_DIR}/include"
    "${PIX_SOURCE_DIR}/src" "${PIX_SOURCE_DIR}/tests")
target_compile_definitions(pix_dt295_stream_retirement_proof PRIVATE
    _POSIX_C_SOURCE=200809L)
target_compile_features(pix_dt295_stream_retirement_proof PRIVATE cxx_std_17)
target_compile_options(pix_dt295_stream_retirement_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
target_link_libraries(pix_dt295_stream_retirement_proof PRIVATE
    pix_dt295_composition Threads::Threads)
target_link_options(pix_dt295_stream_retirement_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer
    -Wl,--wrap=random_bytes
    -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_cancel_transfer
    -Wl,--wrap=libusb_handle_events_timeout_completed
    -Wl,--wrap=libusb_free_transfer
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_stop
    -Wl,--wrap=dt295_stream_destroy
    -Wl,--wrap=dt295_destroy -Wl,--wrap=dt295_routes_destroy
    -Wl,--wrap=dt295_assets_free)
add_test(NAME pix_dt295_stream_retirement_retry
    COMMAND pix_dt295_stream_retirement_proof retry)
add_test(NAME pix_dt295_stream_retirement_detach
    COMMAND pix_dt295_stream_retirement_proof detach)
add_test(NAME pix_dt295_stream_retirement_destructor
    COMMAND pix_dt295_stream_retirement_proof destructor)

add_executable(pix_dt295_acquisition_proof
    "${PIX_SOURCE_DIR}/tests/pix_dt295_acquisition_proof.cpp"
    "${PIX_SOURCE_DIR}/tests/dt295_actual_fixture.c"
    "${PIX_SOURCE_DIR}/tests/dt295_saved_cs_fixture.c")
target_include_directories(pix_dt295_acquisition_proof PRIVATE
    "${PIX_SOURCE_DIR}" "${PIX_SOURCE_DIR}/include"
    "${PIX_SOURCE_DIR}/src" "${PIX_SOURCE_DIR}/tests")
target_compile_definitions(pix_dt295_acquisition_proof PRIVATE
    _POSIX_C_SOURCE=200809L)
target_compile_features(pix_dt295_acquisition_proof PRIVATE cxx_std_17)
target_compile_options(pix_dt295_acquisition_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
target_link_libraries(pix_dt295_acquisition_proof PRIVATE
    pix_dt295_composition Threads::Threads)
target_link_options(pix_dt295_acquisition_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer
    -Wl,--wrap=random_bytes
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=dt295_stream_stop -Wl,--wrap=dt295_stream_destroy
    -Wl,--wrap=dt295_reprepare_plan
    -Wl,--wrap=dt295_routes_feed -Wl,--wrap=dt295_route_write)
add_test(NAME pix_dt295_acquisition_proof
    COMMAND pix_dt295_acquisition_proof)

add_executable(pix_dt295_scan_contract_proof
    "${PIX_SOURCE_DIR}/tests/pix_dt295_scan_contract_proof.cpp"
    "${PIX_SOURCE_DIR}/tests/dt295_actual_fixture.c")
target_include_directories(pix_dt295_scan_contract_proof PRIVATE
    "${PIX_SOURCE_DIR}" "${PIX_SOURCE_DIR}/include"
    "${PIX_SOURCE_DIR}/src" "${PIX_SOURCE_DIR}/tests")
target_compile_definitions(pix_dt295_scan_contract_proof PRIVATE
    _POSIX_C_SOURCE=200809L)
target_compile_features(pix_dt295_scan_contract_proof PRIVATE cxx_std_17)
target_compile_options(pix_dt295_scan_contract_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
target_link_libraries(pix_dt295_scan_contract_proof PRIVATE
    pix_dt295_composition Threads::Threads)
target_link_options(pix_dt295_scan_contract_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer
    -Wl,--wrap=random_bytes
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=dt295_stream_stop -Wl,--wrap=dt295_stream_destroy
    -Wl,--wrap=pthread_join -Wl,--wrap=pthread_create
    -Wl,--wrap=dt295_stop
    -Wl,--wrap=dt295_destroy -Wl,--wrap=dt295_routes_destroy
    -Wl,--wrap=dt295_assets_free)
add_test(NAME pix_dt295_scan_contract_proof
    COMMAND pix_dt295_scan_contract_proof)
add_test(NAME pix_dt295_scan_persistent_close
    COMMAND pix_dt295_scan_contract_proof persistent-close)
add_test(NAME pix_dt295_scan_shutdown_persistent
    COMMAND pix_dt295_scan_contract_proof shutdown-persistent)

add_executable(pix_dt295_live_policy_proof
    "${PIX_SOURCE_DIR}/tests/pix_dt295_live_policy_proof.cpp"
    "${PIX_SOURCE_DIR}/tests/dt295_actual_fixture.c")
target_include_directories(pix_dt295_live_policy_proof PRIVATE
    "${PIX_SOURCE_DIR}" "${PIX_SOURCE_DIR}/include"
    "${PIX_SOURCE_DIR}/src" "${PIX_SOURCE_DIR}/tests")
target_compile_definitions(pix_dt295_live_policy_proof PRIVATE
    _POSIX_C_SOURCE=200809L)
target_compile_features(pix_dt295_live_policy_proof PRIVATE cxx_std_17)
target_compile_options(pix_dt295_live_policy_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
target_link_libraries(pix_dt295_live_policy_proof PRIVATE
    pix_dt295_composition Threads::Threads)
target_link_options(pix_dt295_live_policy_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer
    -Wl,--wrap=random_bytes
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=dt295_stream_stop -Wl,--wrap=dt295_stream_destroy
    -Wl,--wrap=dt295_route_write)
add_test(NAME pix_dt295_live_policy_proof
    COMMAND pix_dt295_live_policy_proof)
endif()

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../include/version.h" PIX_PRODUCT_VERSION_LINE
    REGEX "^#define VERSION \"")
if(NOT PIX_PRODUCT_VERSION_LINE MATCHES
        "^#define VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"$")
    message(FATAL_ERROR "Could not read PIX product version from include/version.h")
endif()
set(PIX_PRODUCT_VERSION "${CMAKE_MATCH_1}")
set(PIX_RUNTIME_VERSION "${PIX_PRODUCT_VERSION}" CACHE STRING "PIX runtime version")
set(PIX_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PIX_FIRMWARE_MANIFEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware-manifest")
set(PIX_FIRMWARE_MANIFEST_HEADER "${PIX_FIRMWARE_MANIFEST_DIR}/pix_firmware_manifest_generated.h")
add_custom_command(
    OUTPUT "${PIX_FIRMWARE_MANIFEST_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${PIX_SOURCE_DIR}/tools/firmware_bundle.py"
        header --output "${PIX_FIRMWARE_MANIFEST_HEADER}"
    DEPENDS "${PIX_SOURCE_DIR}/firmware/manifest.json"
            "${PIX_SOURCE_DIR}/tools/firmware_bundle.py"
    VERBATIM)
if(DEFINED PIX_EMBEDDED_ASSETS OR DEFINED PIX_DT181_FIRMWARE_FILE OR
   DEFINED PIX_DT295_FIRMWARE_FILE OR DEFINED PIX_SOURCE_REVISION_OVERRIDE)
    message(FATAL_ERROR
        "PIX embedded firmware options were removed; pass the bundle path with pixd --firmware FILE")
endif()

add_library(pix_product_runtime STATIC pix_runtime.cpp pix-firmware-bundle.cpp
    "${PIX_FIRMWARE_MANIFEST_HEADER}")
target_include_directories(pix_product_runtime PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}" "${PIX_SOURCE_DIR}"
    PRIVATE "${PIX_SOURCE_DIR}/include" "${PIX_SOURCE_DIR}/src"
    "${PIX_FIRMWARE_MANIFEST_DIR}")
target_compile_definitions(pix_product_runtime PRIVATE
    _POSIX_C_SOURCE=200809L PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}"
    DTV_USERLAND_CORE_VERSION="${DTV_USERLAND_CORE_VERSION}")
target_compile_features(pix_product_runtime PUBLIC cxx_std_17)
target_compile_options(pix_product_runtime PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
target_link_libraries(pix_product_runtime PUBLIC
    pix_dt181_composition pix_dt295_composition
    dtv::host_listener_context dtv::host_runtime dtv::host_facade
    dtv::core dtv::control dtv::identity dtv::tuning
    PkgConfig::LIBUSB Threads::Threads)

add_executable(pixd pixd.cpp)
target_compile_definitions(pixd PRIVATE PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}")
target_compile_features(pixd PRIVATE cxx_std_17)
target_compile_options(pixd PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pixd PRIVATE pix_product_runtime dtv::json PkgConfig::LIBUSB)

add_executable(pix_asset_memory_proof tests/pix_asset_memory_proof.cpp)
target_compile_features(pix_asset_memory_proof PRIVATE cxx_std_17)
target_compile_options(pix_asset_memory_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pix_asset_memory_proof PRIVATE pix_product_runtime)
add_test(NAME pix_asset_memory_proof COMMAND pix_asset_memory_proof)
add_executable(pix_firmware_bundle_proof tests/pix_firmware_bundle_proof.cpp)
target_compile_features(pix_firmware_bundle_proof PRIVATE cxx_std_17)
target_compile_options(pix_firmware_bundle_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pix_firmware_bundle_proof PRIVATE pix_product_runtime)
add_test(NAME pix_firmware_bundle_proof
    COMMAND pix_firmware_bundle_proof
        "${PIX_SOURCE_DIR}/firmware/dt181.bin"
        "${PIX_SOURCE_DIR}/firmware/dt295.bin")
add_executable(pixctl tools/pixctl.cpp)
target_compile_definitions(pixctl PRIVATE
    PX4CTL_PRODUCT_VERSION="${PIX_RUNTIME_VERSION}")
target_compile_features(pixctl PRIVATE cxx_std_17)
target_compile_options(pixctl PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pixctl PRIVATE dtv::host_ctl dtv::json dtv::identity)

add_executable(pix-ts tools/pix_ts.cpp)
target_compile_definitions(pix-ts PRIVATE PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}")
target_compile_features(pix-ts PRIVATE cxx_std_17)
target_compile_options(pix-ts PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pix-ts PRIVATE dtv::host_ts dtv::tuning dtv::host_facade)
add_test(NAME pix_android_launcher
    COMMAND sh "${PIX_SOURCE_DIR}/tests/test_android_launcher.sh" "$<TARGET_FILE:pix-ts>")

add_executable(pix_ts_profile_tests tests/pix_ts_profile_tests.cpp)
target_include_directories(pix_ts_profile_tests PRIVATE tools)
target_compile_features(pix_ts_profile_tests PRIVATE cxx_std_17)
target_compile_options(pix_ts_profile_tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
target_link_libraries(pix_ts_profile_tests PRIVATE dtv::host_ts dtv::tuning)
add_test(NAME pix_ts_profile_tests COMMAND pix_ts_profile_tests)

add_executable(pixd_fixture pixd.cpp
    tests/pix-public-runtime-bottom.c tests/pix-public-runtime-libusb-bottom.c
    "${PIX_SOURCE_DIR}/tests/dt295_actual_fixture.c")
target_include_directories(pixd_fixture PRIVATE
    tests "${PIX_SOURCE_DIR}/tests" "${PIX_SOURCE_DIR}/include" "${PIX_SOURCE_DIR}/src")
target_compile_definitions(pixd_fixture PRIVATE
    _POSIX_C_SOURCE=200809L PIX_CAPTURE_BRIDGE_EXTERNAL
    PIX_TEST_DT295_FIXTURE
    PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}"
    PIX_RUNTIME_TEST_CLOSE_BUDGET_MS=50)
if(NOT WIN32)
    target_compile_definitions(pixd_fixture PRIVATE PIX_TEST_CALLBACK_GATE)
endif()
target_compile_features(pixd_fixture PRIVATE cxx_std_17)
target_compile_options(pixd_fixture PRIVATE "$<$<COMPILE_LANGUAGE:C>:-UNDEBUG>")
target_link_libraries(pixd_fixture PRIVATE pix_product_runtime dtv::json)
target_link_options(pixd_fixture PRIVATE
    -Wl,--wrap=pixy_power -Wl,--wrap=pixy_session -Wl,--wrap=pixy_tuner_init
    -Wl,--wrap=pixy_tune_slot -Wl,--wrap=pixy_status
    -Wl,--wrap=pixy_fifo_clear -Wl,--wrap=pixy_start
    -Wl,--wrap=pixy_stop -Wl,--wrap=pixy_certify -Wl,--wrap=pixy_shutdown
    -Wl,--wrap=libusb_release_interface -Wl,--wrap=libusb_close
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer -Wl,--wrap=random_bytes)
if(WIN32)
    target_link_options(pixd_fixture PRIVATE -Wl,--wrap=CreateThread)
    add_executable(pix_windows_os_thread_proof tests/windows_os_thread_proof.c)
    target_include_directories(pix_windows_os_thread_proof PRIVATE "${PIX_SOURCE_DIR}/include")
    add_test(NAME pix_windows_os_thread_proof COMMAND pix_windows_os_thread_proof)

    add_executable(pix_dt295_stream_windows_i1
        "${PIX_SOURCE_DIR}/tests/test_dt295_stream_i1.c"
        "${PIX_SOURCE_DIR}/src/dt295_stream.c")
    target_include_directories(pix_dt295_stream_windows_i1 PRIVATE
        "${PIX_SOURCE_DIR}/include")
    target_compile_definitions(pix_dt295_stream_windows_i1 PRIVATE
        DT295_STREAM_WINDOWS_ENABLE DT295_STREAM_TESTING)
    target_compile_options(pix_dt295_stream_windows_i1 PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(pix_dt295_stream_windows_i1 PRIVATE PkgConfig::LIBUSB Threads::Threads)

    add_executable(pix_dt295_stream_windows_i2
        "${PIX_SOURCE_DIR}/tests/test_dt295_stream_windows_i2.c"
        "${PIX_SOURCE_DIR}/src/dt295_stream.c")
    target_include_directories(pix_dt295_stream_windows_i2 PRIVATE
        "${PIX_SOURCE_DIR}/include")
    target_compile_definitions(pix_dt295_stream_windows_i2 PRIVATE
        DT295_STREAM_WINDOWS_ENABLE)
    target_compile_options(pix_dt295_stream_windows_i2 PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(pix_dt295_stream_windows_i2 PRIVATE PkgConfig::LIBUSB Threads::Threads)
else()
    target_link_options(pixd_fixture PRIVATE
        -Wl,--wrap=pthread_create -Wl,--wrap=libusb_handle_events_timeout)
endif()

if(NOT WIN32 AND NOT CMAKE_EXE_LINKER_FLAGS MATCHES "-static")
add_executable(pix_public_runtime_proof tests/pix_public_runtime_proof.cpp
    tests/pix-public-runtime-bottom.c tests/pix-public-runtime-libusb-bottom.c)
target_include_directories(pix_public_runtime_proof PRIVATE
    tests "${CMAKE_CURRENT_LIST_DIR}" "${PIX_SOURCE_DIR}/tests"
    "${PIX_SOURCE_DIR}/include" "${PIX_SOURCE_DIR}/src")
target_compile_definitions(pix_public_runtime_proof PRIVATE
    _POSIX_C_SOURCE=200809L PIX_RUNTIME_VERSION="${PIX_RUNTIME_VERSION}"
    DTV_USERLAND_CORE_VERSION="${DTV_USERLAND_CORE_VERSION}"
    PIXD_PATH="$<TARGET_FILE:pixd_fixture>"
    PIX_FIRMWARE_DT181_PATH="${PIX_SOURCE_DIR}/firmware/dt181.bin"
    PIX_FIRMWARE_DT295_PATH="${PIX_SOURCE_DIR}/firmware/dt295.bin"
    PIXTS_PATH="$<TARGET_FILE:pix-ts>" PIXCTL_PATH="$<TARGET_FILE:pixctl>")
target_compile_features(pix_public_runtime_proof PRIVATE cxx_std_17)
target_link_libraries(pix_public_runtime_proof PRIVATE pix_product_runtime Threads::Threads)
target_link_options(pix_public_runtime_proof PRIVATE
    -Wl,--wrap=pixy_power -Wl,--wrap=pixy_session -Wl,--wrap=pixy_tuner_init
    -Wl,--wrap=pixy_tune_slot -Wl,--wrap=pixy_status
    -Wl,--wrap=pixy_fifo_clear -Wl,--wrap=pixy_start
    -Wl,--wrap=pixy_stop -Wl,--wrap=pixy_certify -Wl,--wrap=pixy_shutdown
    -Wl,--wrap=libusb_release_interface -Wl,--wrap=libusb_close
    -Wl,--wrap=pthread_create)
add_dependencies(pix_public_runtime_proof pixd_fixture pix-ts pixctl)
add_test(NAME pix_public_runtime_proof COMMAND pix_public_runtime_proof)
endif()

if(NOT WIN32)
add_executable(pix_dt295_family_contract_proof
    tests/pix_dt295_family_contract_proof.cpp
    "${PIX_SOURCE_DIR}/tests/dt295_actual_fixture.c")
target_include_directories(pix_dt295_family_contract_proof PRIVATE
    tests "${CMAKE_CURRENT_LIST_DIR}" "${PIX_SOURCE_DIR}/tests"
    "${PIX_SOURCE_DIR}/include" "${PIX_SOURCE_DIR}/src")
target_compile_definitions(pix_dt295_family_contract_proof PRIVATE
    _POSIX_C_SOURCE=200809L
    PIX_FIRMWARE_DT295_PATH="${PIX_SOURCE_DIR}/firmware/dt295.bin")
target_compile_features(pix_dt295_family_contract_proof PRIVATE cxx_std_17)
target_compile_options(pix_dt295_family_contract_proof PRIVATE
    -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(pix_dt295_family_contract_proof PRIVATE
    pix_product_runtime PkgConfig::LIBUSB Threads::Threads)
target_link_options(pix_dt295_family_contract_proof PRIVATE
    -Wl,--wrap=usb_open_model -Wl,--wrap=usb_close
    -Wl,--wrap=usb_native_context -Wl,--wrap=usb_native_handle
    -Wl,--wrap=libusb_control_transfer -Wl,--wrap=libusb_bulk_transfer
    -Wl,--wrap=libusb_interrupt_transfer
    -Wl,--wrap=random_bytes
    -Wl,--wrap=dt295_stream_start -Wl,--wrap=dt295_stream_read
    -Wl,--wrap=dt295_stream_stop -Wl,--wrap=dt295_stream_destroy)
add_test(NAME pix_dt295_family_contract_proof
    COMMAND pix_dt295_family_contract_proof)
endif()

# Optional PIX L3 adapter skeleton. It consumes an external Core prototype
# header/source candidate (fixed ref 8bf496c9 at the time of writing) and does
# not change the existing CLI, daemon or custom-FW targets.
option(PIX_BUILD_L3_ADAPTER "Build the optional PIX L3 product adapter" OFF)
set(PIX_L3_CORE_PROTOTYPE_DIR "" CACHE PATH
    "Core prototype/l3_owner directory for the PIX L3 adapter")
if(PIX_BUILD_L3_ADAPTER)
    if(NOT PIX_L3_CORE_PROTOTYPE_DIR)
        set(PIX_L3_CORE_PROTOTYPE_DIR
            "${PIX_CORE_SOURCE_DIR}/prototype/l3_owner")
    endif()
    if(NOT EXISTS "${PIX_L3_CORE_PROTOTYPE_DIR}/include/dtv/l3_owner.h")
        message(FATAL_ERROR
            "PIX L3 adapter needs PIX_L3_CORE_PROTOTYPE_DIR with include/dtv/l3_owner.h")
    endif()
    add_subdirectory("${PIX_L3_CORE_PROTOTYPE_DIR}"
        "${CMAKE_BINARY_DIR}/pix-l3-core-prototype")

    add_library(pix_l3_adapter STATIC
        "${CMAKE_CURRENT_LIST_DIR}/pix_l3_adapter.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/pix_l3_dt295_io_backend.cpp")
    target_compile_features(pix_l3_adapter PUBLIC cxx_std_17)
    target_include_directories(pix_l3_adapter PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}" "${PIX_SOURCE_DIR}/include")
    target_compile_options(pix_l3_adapter PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
    target_link_libraries(pix_l3_adapter PUBLIC
        l3_owner_model Threads::Threads)

    add_library(pix_l3_dt181_backend STATIC
        "${CMAKE_CURRENT_LIST_DIR}/pix_l3_dt181_backend.cpp")
    target_compile_features(pix_l3_dt181_backend PUBLIC cxx_std_17)
    target_include_directories(pix_l3_dt181_backend PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_options(pix_l3_dt181_backend PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
    target_link_libraries(pix_l3_dt181_backend PUBLIC
        pix_l3_adapter pix_dt181_c_backend)

    add_executable(pix_l3_c_consumer
        "${CMAKE_CURRENT_LIST_DIR}/tests/pix_l3_c_consumer.c")
    target_compile_options(pix_l3_c_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set_target_properties(pix_l3_c_consumer PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(pix_l3_c_consumer PRIVATE pix_l3_adapter)
    add_test(NAME pix_l3_c_consumer COMMAND pix_l3_c_consumer)

    add_executable(pix_l3_adapter_contract
        "${CMAKE_CURRENT_LIST_DIR}/tests/pix_l3_adapter_contract.cpp")
    target_compile_features(pix_l3_adapter_contract PRIVATE cxx_std_17)
    target_compile_options(pix_l3_adapter_contract PRIVATE
        -UNDEBUG -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
    target_link_libraries(pix_l3_adapter_contract PRIVATE
        pix_l3_adapter Threads::Threads)
    add_test(NAME pix_l3_adapter_contract COMMAND pix_l3_adapter_contract)

    add_executable(pix_l3_dt295_io_backend_contract
        "${CMAKE_CURRENT_LIST_DIR}/tests/pix_l3_dt295_io_backend_contract.cpp")
    target_compile_features(pix_l3_dt295_io_backend_contract PRIVATE cxx_std_17)
    target_compile_options(pix_l3_dt295_io_backend_contract PRIVATE
        -UNDEBUG -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
    target_link_libraries(pix_l3_dt295_io_backend_contract PRIVATE
        pix_l3_adapter Threads::Threads)
    add_test(NAME pix_l3_dt295_io_backend_contract
        COMMAND pix_l3_dt295_io_backend_contract)
endif()
