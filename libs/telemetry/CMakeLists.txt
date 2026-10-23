add_library(telemetry STATIC src/Logger.cpp)

target_include_directories(telemetry PUBLIC include)
apply_defaults(telemetry)
