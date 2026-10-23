add_library(gateway-config STATIC src/GatewayConfig.cpp src/DbCredentialProvider.cpp)
target_include_directories(gateway-config PUBLIC src)
target_link_libraries(gateway-config PUBLIC domain dahua-adapter persistence security)
apply_defaults(gateway-config)

add_executable(dahua-gateway src/main.cpp)
target_link_libraries(dahua-gateway PRIVATE gateway-config dahua-adapter persistence security telemetry domain)
apply_defaults(dahua-gateway)
deploy_dahua_runtime(dahua-gateway)
