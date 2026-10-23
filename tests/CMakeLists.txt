add_executable(unit-tests
    unit/DeviceSessionManagerTests.cpp
    unit/DhavFixtureTests.cpp
    unit/DhavParserTests.cpp
    unit/G711Tests.cpp
    unit/GatewayConfigTests.cpp
    unit/LoggerTests.cpp
    unit/LoginErrorsTests.cpp
    unit/LoginPolicyTests.cpp
    unit/LoginThrottleTests.cpp
    unit/PersistenceTests.cpp
    unit/ResultTests.cpp
    unit/SecurityTests.cpp
    unit/support/DhavFrameBuilder.cpp
)

target_include_directories(unit-tests PRIVATE unit/support)
target_compile_definitions(unit-tests PRIVATE SURVEILLANCE_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations")

target_link_libraries(unit-tests PRIVATE
    persistence
    security
    dahua-adapter
    telemetry
    domain
    gateway-config
    GTest::gtest
    GTest::gtest_main
)

apply_defaults(unit-tests)
deploy_dahua_runtime(unit-tests)

include(GoogleTest)
gtest_discover_tests(unit-tests DISCOVERY_MODE PRE_TEST)
