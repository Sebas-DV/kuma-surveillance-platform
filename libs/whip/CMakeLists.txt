find_package(LibDataChannel CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(whip STATIC src/Publisher.cpp)

target_include_directories(whip PUBLIC include PRIVATE src)
target_link_libraries(whip
    PUBLIC domain dahua-adapter Threads::Threads
    PRIVATE LibDataChannel::LibDataChannel Drogon::Drogon
)
apply_defaults(whip)
