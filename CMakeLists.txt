cmake_minimum_required(VERSION 3.25)

project(
    SurveillancePlatform
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(SURVEILLANCE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SURVEILLANCE_BUILD_TESTS "Build unit tests" ON)
option(SURVEILLANCE_BUILD_CORE_API "Build the Drogon core API" ON)

include(TargetDefaults)
include(DahuaNetSdk)

find_package(Drogon CONFIG REQUIRED)

add_subdirectory(libs/domain)
add_subdirectory(libs/telemetry)
add_subdirectory(libs/security)
add_subdirectory(libs/persistence)
add_subdirectory(libs/dahua-adapter)
add_subdirectory(apps/dahua-gateway)

if(SURVEILLANCE_BUILD_CORE_API)
    add_subdirectory(apps/core-api)
endif()

if(SURVEILLANCE_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
