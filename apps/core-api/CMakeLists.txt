add_executable(surveillance-core
    src/main.cpp
    src/Routes.cpp
    src/Http.cpp
    src/Serialization.cpp
)

target_include_directories(surveillance-core PRIVATE include src)
target_link_libraries(surveillance-core PRIVATE
    Drogon::Drogon
    persistence
    security
    telemetry
    domain
)
apply_defaults(surveillance-core)
