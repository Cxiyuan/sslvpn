# --------------------------------------------------------------------------------------------------
# spdlog
# --------------------------------------------------------------------------------------------------
if(APPLE)
    ExternalProject_Add(spdlog-${spdlog-TAG}
        PREFIX ${CMAKE_BINARY_DIR}/external
        
        INSTALL_DIR ${CMAKE_BINARY_DIR}/external
        
        UPDATE_DISCONNECTED 0
        UPDATE_COMMAND ""
        
        GIT_REPOSITORY https://github.com/gabime/spdlog
        GIT_TAG ${spdlog-TAG}
        GIT_SHALLOW 1

        CMAKE_ARGS "${CMAKE_ARGS};-DCMAKE_BUILD_TYPE=Release;-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>;-DSPDLOG_BUILD_BENCH=off;-DSPDLOG_BUILD_EXAMPLES=off;-DSPDLOG_BUILD_TESTS=off;-DBUILD_TESTING=off;-DCMAKE_POLICY_VERSION_MINIMUM=3.5;-DSPDLOG_FMT_EXTERNAL=OFF;-DSPDLOG_BUILD_SHARED=OFF"
        CMAKE_COMMAND ${CMAKE_CROSS_COMMAND}
        BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/external/lib/libspdlog.a
    )
    
    add_library(spdlog::spdlog STATIC IMPORTED)
    set_target_properties(spdlog::spdlog PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/external/include"
        IMPORTED_LOCATION ${CMAKE_BINARY_DIR}/external/lib/libspdlog.a
    )
    add_dependencies(spdlog::spdlog DEPENDS spdlog-${spdlog-TAG})
else()
    ExternalProject_Add(spdlog-${spdlog-TAG}
        PREFIX ${CMAKE_BINARY_DIR}/external
        
        INSTALL_DIR ${CMAKE_BINARY_DIR}/external
        
        UPDATE_DISCONNECTED 0
        UPDATE_COMMAND ""
        
        GIT_REPOSITORY https://github.com/gabime/spdlog
        GIT_TAG ${spdlog-TAG}
        GIT_SHALLOW 1

        CMAKE_ARGS "${CMAKE_ARGS};-DCMAKE_BUILD_TYPE=Release;-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>;-DSPDLOG_BUILD_BENCH=off;-DSPDLOG_BUILD_EXAMPLES=off;-DSPDLOG_BUILD_TESTS=off;-DBUILD_TESTING=off;-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        CMAKE_COMMAND ${CMAKE_CROSS_COMMAND}
    )
    
    add_library(spdlog::spdlog INTERFACE IMPORTED)
    set_target_properties(spdlog::spdlog PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/external/include"
    )
    add_dependencies(spdlog::spdlog DEPENDS spdlog-${spdlog-TAG})
endif()