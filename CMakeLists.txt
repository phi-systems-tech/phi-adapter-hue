cmake_minimum_required(VERSION 3.21)

project(phi-adapter-hue
    VERSION 1.0.0
    DESCRIPTION "Phi Philips Hue adapter plugin"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

set(PHI_ADAPTER_API_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-api" CACHE PATH
    "Path to local phi-adapter-api checkout. If not found, find_package(phi-adapter-api) is used."
)

if(EXISTS "${PHI_ADAPTER_API_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "Using local phi-adapter-api from: ${PHI_ADAPTER_API_SOURCE_DIR}")
    add_subdirectory("${PHI_ADAPTER_API_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-api")
    set(PHI_ADAPTER_API_INCLUDE_DIR "${PHI_ADAPTER_API_SOURCE_DIR}/include/phi/adapter/api")
else()
    message(STATUS "Using installed phi-adapter-api via find_package")
    find_package(phi-adapter-api CONFIG REQUIRED)
    find_path(PHI_ADAPTER_API_INCLUDE_DIR
        NAMES adapterinterface.h
        PATH_SUFFIXES phi/adapter/api
    )
endif()

if(NOT TARGET phi::adapter-api)
    message(FATAL_ERROR "phi-adapter-api target phi::adapter-api not found")
endif()

if(NOT PHI_ADAPTER_API_INCLUDE_DIR)
    message(FATAL_ERROR "Could not resolve phi-adapter-api header include directory")
endif()

add_library(phi_adapter_hue MODULE
    src/hueadapter.cpp
    src/hueadapter.h
    src/hueadapterfactory.cpp
    src/hueadapterfactory.h
    ${PHI_ADAPTER_API_INCLUDE_DIR}/adapterinterface.h
    ${PHI_ADAPTER_API_INCLUDE_DIR}/adapterfactory.h
)

target_include_directories(phi_adapter_hue
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(phi_adapter_hue
    PRIVATE
        Qt6::Core
        Qt6::Network
        phi::adapter-api
)

set_target_properties(phi_adapter_hue PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters"
)

add_custom_command(
    TARGET phi_adapter_hue POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/hue-config.json"
        "$<TARGET_FILE_DIR:phi_adapter_hue>/hue-config.json"
)

install(TARGETS phi_adapter_hue
    LIBRARY DESTINATION lib/phi/plugins/adapters
)

install(FILES hue-config.json
    DESTINATION lib/phi/plugins/adapters
)
