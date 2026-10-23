add_library(domain STATIC src/Result.cpp src/Environment.cpp)

target_include_directories(domain PUBLIC include)
apply_defaults(domain)
