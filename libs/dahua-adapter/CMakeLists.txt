add_library(dahua-adapter STATIC
    src/DeviceSessionManager.cpp
    src/DhavParser.cpp
    src/G711.cpp
    src/LoginErrors.cpp
    src/LoginPolicy.cpp
    src/MediaPacket.cpp
    src/NetSdkRuntime.cpp
    src/RegistrationServer.cpp
    src/SdkSessionFactory.cpp
    src/Types.cpp
    src/detail/Secure.cpp
    src/detail/SdkSupport.cpp
)

target_include_directories(dahua-adapter
    PUBLIC include
    PRIVATE src
)

target_link_libraries(dahua-adapter
    PUBLIC domain telemetry
    PRIVATE Dahua::NetSdk
)

find_package(Threads REQUIRED)
target_link_libraries(dahua-adapter PUBLIC Threads::Threads)

apply_defaults(dahua-adapter)
