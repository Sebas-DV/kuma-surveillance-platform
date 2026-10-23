set(SURVEILLANCE_DAHUA_ROOT "${CMAKE_SOURCE_DIR}/third_party/dahua" CACHE PATH "Dahua NetSDK root")

if(NOT EXISTS "${SURVEILLANCE_DAHUA_ROOT}/include/dhnetsdk.h")
    message(FATAL_ERROR "dhnetsdk.h not found in ${SURVEILLANCE_DAHUA_ROOT}/include")
endif()

if(NOT EXISTS "${SURVEILLANCE_DAHUA_ROOT}/lib/dhnetsdk.lib")
    message(FATAL_ERROR "dhnetsdk.lib not found in ${SURVEILLANCE_DAHUA_ROOT}/lib")
endif()

add_library(Dahua::NetSdk INTERFACE IMPORTED)
set_target_properties(Dahua::NetSdk PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SURVEILLANCE_DAHUA_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "${SURVEILLANCE_DAHUA_ROOT}/lib/dhnetsdk.lib"
)

function(deploy_dahua_runtime target)
    if(EXISTS "${SURVEILLANCE_DAHUA_ROOT}/bin")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${SURVEILLANCE_DAHUA_ROOT}/bin" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
        )
    endif()
endfunction()
