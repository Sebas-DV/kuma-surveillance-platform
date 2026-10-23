add_library(persistence STATIC
    src/Database.cpp
    src/DriverLog.cpp
    src/Rows.cpp
    src/Migrator.cpp
    src/DeviceRepository.cpp
    src/UserRepository.cpp
    src/ApiKeyRepository.cpp
    src/VideoTokenRepository.cpp
    src/Models.cpp
    src/detail/Sql.cpp
)

target_include_directories(persistence
    PUBLIC include
    PRIVATE src
)
target_link_libraries(persistence PUBLIC domain security Drogon::Drogon)
apply_defaults(persistence)
