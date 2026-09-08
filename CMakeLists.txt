cmake_minimum_required(VERSION 3.21)

project(phi-adapter-hue
    VERSION 2.0.0
    DESCRIPTION "Phi Philips Hue adapter"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No Qt. The loop, the timers and the strings come from phi-runtime through the
# SDK; HTTPS and the event stream from the SDK's own client; JSON from
# nlohmann, which is what phi-core uses.
find_package(nlohmann_json 3.11 REQUIRED)

option(PHI_ADAPTER_HUE_BUILD_IPC "Build IPC sidecar executable for hue adapter" ON)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout. If not found, find_package(phi-adapter-sdk) is used."
)
option(PHI_ADAPTER_HUE_USE_LOCAL_ADAPTER_SDK
    "Use local ../phi-adapter-sdk checkout when available" ON)

# Signify's root, which signs every bridge's certificate. Installed with the
# package and named to the adapter at compile time; the meta's tlsCaFile
# replaces it for anybody who has a reason to.
set(PHI_HUE_CA_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/phi/adapters/hue")
set(PHI_HUE_CA_FILE "${CMAKE_INSTALL_PREFIX}/${PHI_HUE_CA_INSTALL_DIR}/huebridge_cacert.pem")

if(PHI_ADAPTER_HUE_BUILD_IPC)
    if(NOT TARGET phi::adapter-sdk)
        if(PHI_ADAPTER_HUE_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk from: ${PHI_ADAPTER_SDK_SOURCE_DIR}")
            set(PHI_ADAPTER_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
        else()
            message(STATUS "Using installed phi-adapter-sdk via find_package")
            find_package(phi-adapter-sdk CONFIG REQUIRED)
        endif()
    endif()

    if(NOT TARGET phi::adapter-sdk)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk not found")
    endif()

    # One concern per translation unit; everything above the instance is pure
    # enough to test without a bridge.
    add_library(phi_adapter_hue_core STATIC
        src/hue_events.cpp
        src/hue_instance.cpp
        src/hue_json.cpp
        src/hue_model.cpp
        src/hue_probe.cpp
        src/hue_schema.cpp
        src/hue_settings.cpp
    )
    target_compile_features(phi_adapter_hue_core PUBLIC cxx_std_20)
    set_target_properties(phi_adapter_hue_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(phi_adapter_hue_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(phi_adapter_hue_core PRIVATE HUE_BRIDGE_CA_FILE="${PHI_HUE_CA_FILE}")
    target_link_libraries(phi_adapter_hue_core
        PUBLIC
            phi::adapter-sdk
            nlohmann_json::nlohmann_json
    )

    add_executable(phi_adapter_hue_ipc src/main.cpp)
    target_link_libraries(phi_adapter_hue_ipc PRIVATE phi_adapter_hue_core)

    enable_testing()

    # A bridge's answers and what they become; the event stream taken apart;
    # a poll that could not fetch everything.
    add_executable(hue_model_tests tests/hue_model_tests.cpp)
    target_link_libraries(hue_model_tests PRIVATE phi_adapter_hue_core)
    add_test(NAME hue_model_tests COMMAND hue_model_tests)
    set_tests_properties(hue_model_tests PROPERTIES TIMEOUT 120)

    set_target_properties(phi_adapter_hue_ipc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters"
    )

    install(TARGETS phi_adapter_hue_ipc
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/huebridge_cacert.pem"
        DESTINATION ${PHI_HUE_CA_INSTALL_DIR}
    )
endif()
