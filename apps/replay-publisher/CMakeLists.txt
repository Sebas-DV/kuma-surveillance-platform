add_executable(replay-publisher src/main.cpp)
target_link_libraries(replay-publisher PRIVATE whip dahua-adapter domain telemetry)
apply_defaults(replay-publisher)
