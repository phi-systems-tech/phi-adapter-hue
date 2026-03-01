cmake_minimum_required(VERSION 3.21)

project(phi-adapter-hue
    VERSION 1.0.0
    DESCRIPTION "Phi Philips Hue adapter"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

option(PHI_ADAPTER_HUE_BUILD_IPC
    "Build IPC sidecar executable for hue adapter"
    ON
)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout. If not found, find_package(phi-adapter-sdk) is used."
)
option(PHI_ADAPTER_HUE_USE_LOCAL_ADAPTER_SDK
    "Use local ../phi-adapter-sdk checkout when available"
    ON
)

if(PHI_ADAPTER_HUE_BUILD_IPC)
    if(NOT TARGET phi::adapter-sdk)
        if(PHI_ADAPTER_HUE_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk from: ${PHI_ADAPTER_SDK_SOURCE_DIR}")
            add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
        else()
            message(STATUS "Using installed phi-adapter-sdk via find_package")
            find_package(phi-adapter-sdk CONFIG REQUIRED)
        endif()
    endif()

    if(NOT TARGET phi::adapter-sdk)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk not found")
    endif()

    add_executable(phi_adapter_hue_ipc
        src/main.cpp
        src/hue_http.cpp
        src/hue_model.cpp
        src/hue_probe.cpp
        src/hue_schema.cpp
        src/hue_sidecar.cpp
    )

    target_compile_features(phi_adapter_hue_ipc PRIVATE cxx_std_20)

    target_include_directories(phi_adapter_hue_ipc
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(phi_adapter_hue_ipc
        PRIVATE
            Qt6::Core
            Qt6::Network
            phi::adapter-sdk
    )

    set_target_properties(phi_adapter_hue_ipc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters"
    )

    install(TARGETS phi_adapter_hue_ipc
        RUNTIME DESTINATION lib/phi/plugins/adapters
    )
endif()
