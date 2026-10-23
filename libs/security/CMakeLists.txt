find_package(unofficial-sodium CONFIG REQUIRED)

add_library(security STATIC
    src/Crypto.cpp
    src/LoginThrottle.cpp
    src/PasswordHasher.cpp
    src/SecretBox.cpp
)

target_include_directories(security PUBLIC include)
target_link_libraries(security PUBLIC domain PRIVATE unofficial-sodium::sodium)
apply_defaults(security)
