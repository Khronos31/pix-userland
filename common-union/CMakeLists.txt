cmake_minimum_required(VERSION 3.16)
project(dtv_userland_core VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
    set(_dtv_userland_testing_default ${BUILD_TESTING})
else()
    set(_dtv_userland_testing_default OFF)
endif()
option(DTV_USERLAND_BUILD_TESTING "Build DTV userland core tests" ${_dtv_userland_testing_default})
# The internal receiver lifecycle engine is an opt-in target so that default
# core and wire configurations keep their dependency footprint; only it
# requires Threads.
option(DTV_USERLAND_BUILD_RECEIVER "Build the internal receiver lifecycle engine" OFF)
option(DTV_USERLAND_BUILD_CARD "Build the internal card ownership engine" OFF)
# The internal packet-session storage is opt-in and, like the receiver engine,
# is one of the only targets that requires Threads.  It does not depend on the
# receiver engine, so a capture-only install never needs the receiver library.
option(DTV_USERLAND_BUILD_CAPTURE "Build the internal packet-session storage" OFF)

add_library(dtv_userland_core STATIC
    src/error.cpp
    src/logging.cpp
    src/mock_transport.cpp)
add_library(dtv::core ALIAS dtv_userland_core)
set_target_properties(dtv_userland_core PROPERTIES
    EXPORT_NAME core
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_core PUBLIC cxx_std_17)
target_include_directories(dtv_userland_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(dtv_userland_wire STATIC src/wire.cpp src/records.cpp)
add_library(dtv::wire ALIAS dtv_userland_wire)
set_target_properties(dtv_userland_wire PROPERTIES
    EXPORT_NAME wire
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_wire PUBLIC cxx_std_17)
target_include_directories(dtv_userland_wire PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_wire PUBLIC dtv::core)

add_library(dtv_userland_identity STATIC src/identity.cpp)
add_library(dtv::identity ALIAS dtv_userland_identity)
set_target_properties(dtv_userland_identity PROPERTIES
    EXPORT_NAME identity
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_identity PUBLIC cxx_std_17)
target_include_directories(dtv_userland_identity PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_identity PUBLIC dtv::core)

add_library(dtv_userland_tuning STATIC src/tuning.cpp)
add_library(dtv::tuning ALIAS dtv_userland_tuning)
set_target_properties(dtv_userland_tuning PROPERTIES
    EXPORT_NAME tuning
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_tuning PUBLIC cxx_std_17)
target_include_directories(dtv_userland_tuning PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_tuning PUBLIC dtv::wire)

add_library(dtv_userland_json STATIC src/json.cpp)
add_library(dtv::json ALIAS dtv_userland_json)
set_target_properties(dtv_userland_json PROPERTIES
    EXPORT_NAME json
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_json PUBLIC cxx_std_17)
target_include_directories(dtv_userland_json PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_json PUBLIC dtv::wire)

# Portable UDTV control-connection serializer shared by the product daemons.
add_library(dtv_userland_control STATIC src/control_connection.cpp src/control_client.cpp)
add_library(dtv::control ALIAS dtv_userland_control)
set_target_properties(dtv_userland_control PROPERTIES
    EXPORT_NAME control
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_control PUBLIC cxx_std_17)
target_include_directories(dtv_userland_control PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_control PUBLIC dtv::wire)

if(MSVC)
    target_compile_options(dtv_userland_core PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_wire PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_identity PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_tuning PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_json PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_control PRIVATE /EHs-c- /GR- /W4)
else()
    target_compile_options(dtv_userland_core PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_wire PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_identity PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_tuning PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_json PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_control PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
endif()

if(DTV_USERLAND_BUILD_RECEIVER)
    find_package(Threads REQUIRED)
    add_library(dtv_userland_receiver STATIC src/receiver_engine.cpp)
    add_library(dtv::receiver ALIAS dtv_userland_receiver)
    set_target_properties(dtv_userland_receiver PROPERTIES
        EXPORT_NAME receiver
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_receiver PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_receiver PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_receiver PUBLIC dtv::core Threads::Threads)
    if(MSVC)
        target_compile_options(dtv_userland_receiver PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_receiver PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()

if(DTV_USERLAND_BUILD_CAPTURE)
    find_package(Threads REQUIRED)
    add_library(dtv_userland_capture STATIC src/packet_stream_session.cpp)
    add_library(dtv::capture ALIAS dtv_userland_capture)
    set_target_properties(dtv_userland_capture PROPERTIES
        EXPORT_NAME capture
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_capture PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_capture PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    # The capture component uses the shared neutral session types and the
    # error/result foundation only; the disabled receiver engine is not a
    # dependency.
    target_link_libraries(dtv_userland_capture PUBLIC dtv::core Threads::Threads)
    if(MSVC)
        target_compile_options(dtv_userland_capture PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_capture PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()

if(DTV_USERLAND_BUILD_CARD)
    add_library(dtv_userland_card INTERFACE)
    add_library(dtv::card ALIAS dtv_userland_card)
    set_target_properties(dtv_userland_card PROPERTIES EXPORT_NAME card)
    target_compile_features(dtv_userland_card INTERFACE cxx_std_17)
    target_include_directories(dtv_userland_card INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_card INTERFACE dtv::core)
endif()

install(TARGETS dtv_userland_core dtv_userland_wire dtv_userland_identity dtv_userland_tuning dtv_userland_json
        dtv_userland_control
    EXPORT dtv_userland_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(DTV_USERLAND_BUILD_CARD)
    install(TARGETS dtv_userland_card EXPORT dtv_userland_coreTargets)
endif()
# The top-level headers are always installed.  The detail headers are selected
# so a disabled engine's header never leaks into a consumer: the receiver header
# only when the receiver is enabled, the capture header only when capture is
# enabled, and the shared neutral session type header when either needs it.
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "detail" EXCLUDE)
# The shared identity/wire internals are always installed.
install(FILES
    include/dtv/userland/detail/text.h
    include/dtv/userland/detail/sha256.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
if(DTV_USERLAND_BUILD_RECEIVER OR DTV_USERLAND_BUILD_CAPTURE)
    install(FILES
        include/dtv/userland/detail/stream_session_types.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
if(DTV_USERLAND_BUILD_RECEIVER)
    install(FILES
        include/dtv/userland/detail/receiver_engine.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
if(DTV_USERLAND_BUILD_CAPTURE)
    install(FILES
        include/dtv/userland/detail/packet_stream_session.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
if(DTV_USERLAND_BUILD_CARD)
    install(FILES
        include/dtv/userland/detail/card_ownership_engine.h
        include/dtv/userland/detail/card_ownership_engine.tpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
install(EXPORT dtv_userland_coreTargets
    FILE dtv_userland_coreTargets.cmake
    NAMESPACE dtv::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
if(DTV_USERLAND_BUILD_RECEIVER)
    # Loaded only for find_package(... COMPONENTS receiver), which also finds
    # Threads for the consumer.
    install(TARGETS dtv_userland_receiver
        EXPORT dtv_userland_receiverTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT dtv_userland_receiverTargets
        FILE dtv_userland_receiverTargets.cmake
        NAMESPACE dtv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
endif()
if(DTV_USERLAND_BUILD_CAPTURE)
    # Loaded only for find_package(... COMPONENTS capture), which also finds
    # Threads for the consumer.
    install(TARGETS dtv_userland_capture
        EXPORT dtv_userland_captureTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT dtv_userland_captureTargets
        FILE dtv_userland_captureTargets.cmake
        NAMESPACE dtv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
endif()
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/dtv_userland_coreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dtv_userland_core)

if(DTV_USERLAND_BUILD_TESTING)
    add_executable(foundation_tests tests/foundation_tests.cpp)
    target_link_libraries(foundation_tests PRIVATE dtv::core)
    add_test(NAME foundation_tests COMMAND foundation_tests)

    add_executable(wire_tests tests/wire_tests.cpp)
    target_link_libraries(wire_tests PRIVATE dtv::wire)
    add_test(NAME wire_tests COMMAND wire_tests)

    add_executable(records_tests tests/records_tests.cpp)
    target_link_libraries(records_tests PRIVATE dtv::wire)
    add_test(NAME records_tests COMMAND records_tests)

    add_executable(identity_tests tests/identity_tests.cpp)
    target_link_libraries(identity_tests PRIVATE dtv::identity)
    add_test(NAME identity_tests COMMAND identity_tests)

    add_executable(tuning_tests tests/tuning_tests.cpp)
    target_link_libraries(tuning_tests PRIVATE dtv::tuning)
    add_test(NAME tuning_tests COMMAND tuning_tests)

    add_executable(control_connection_tests tests/control_connection_tests.cpp)
    target_link_libraries(control_connection_tests PRIVATE dtv::control)
    add_test(NAME control_connection_tests COMMAND control_connection_tests)

    add_executable(control_client_tests tests/control_client_tests.cpp)
    target_link_libraries(control_client_tests PRIVATE dtv::control)
    add_test(NAME control_client_tests COMMAND control_client_tests)

    add_executable(status_json_tests tests/status_json_tests.cpp)
    target_link_libraries(status_json_tests PRIVATE dtv::json)
    add_test(NAME status_json_tests COMMAND status_json_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME status_json_consumer
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/status_json_consumer.py
            $<TARGET_FILE:status_json_tests>)

    if(DTV_USERLAND_BUILD_RECEIVER)
        add_executable(receiver_engine_tests tests/receiver_engine_tests.cpp)
        target_link_libraries(receiver_engine_tests PRIVATE dtv::receiver)
        add_test(NAME receiver_engine_tests COMMAND receiver_engine_tests)
        # Outer bound behind the tests' own finite waits.
        set_tests_properties(receiver_engine_tests PROPERTIES TIMEOUT 120)

        add_executable(receiver_r2_tests tests/receiver_r2_tests.cpp)
        target_link_libraries(receiver_r2_tests PRIVATE dtv::receiver)
        # Test-binary-only real-mutex observer for the registry rendezvous
        # proofs.  Linux GNU/Clang only; other platforms compile it out and the
        # portable tests remain unchanged.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
           AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_definitions(receiver_r2_tests PRIVATE DTV_R2_LOCK_OBSERVER=1)
            target_link_options(receiver_r2_tests PRIVATE
                "LINKER:--wrap=pthread_mutex_lock")
        endif()
        add_test(NAME receiver_r2_tests COMMAND receiver_r2_tests)
        set_tests_properties(receiver_r2_tests PROPERTIES TIMEOUT 120)

        add_executable(receiver_hz_tests tests/receiver_hz_tests.cpp)
        target_link_libraries(receiver_hz_tests PRIVATE dtv::receiver)
        add_test(NAME receiver_hz_tests COMMAND receiver_hz_tests)
        set_tests_properties(receiver_hz_tests PROPERTIES TIMEOUT 120)
    endif()

    if(DTV_USERLAND_BUILD_CAPTURE)
        add_executable(packet_stream_session_tests
            tests/packet_stream_session_tests.cpp)
        target_link_libraries(packet_stream_session_tests PRIVATE dtv::capture)
        add_test(NAME packet_stream_session_tests
            COMMAND packet_stream_session_tests)
        # Outer bound behind the tests' own finite waits.
        set_tests_properties(packet_stream_session_tests PROPERTIES TIMEOUT 120)
    endif()

    if(DTV_USERLAND_BUILD_CARD)
        add_executable(card_ownership_engine_tests tests/card_ownership_engine_tests.cpp)
        target_link_libraries(card_ownership_engine_tests PRIVATE dtv::card)
        if(MSVC)
            target_compile_options(card_ownership_engine_tests PRIVATE /EHs-c- /GR- /W4)
        else()
            target_compile_options(card_ownership_engine_tests PRIVATE
                -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME card_ownership_engine_tests COMMAND card_ownership_engine_tests)
        set_tests_properties(card_ownership_engine_tests PROPERTIES TIMEOUT 120)
    endif()

    set(DTV_LEGACY_SOURCE "" CACHE PATH "Fixed PX4 userland source tree for equivalence check")
    if(DTV_LEGACY_SOURCE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME legacy_equivalence
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_equivalence.py
                --source ${DTV_LEGACY_SOURCE} --cxx ${CMAKE_CXX_COMPILER})
    endif()
endif()
