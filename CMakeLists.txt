cmake_minimum_required(VERSION 3.12 FATAL_ERROR)

if(APPLE)
     if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "" FORCE)
    endif()
endif()

project(avpn
    VERSION 1.5.3
    LANGUAGES CXX
)

message(STATUS "Building ${PROJECT_NAME} v${PROJECT_VERSION} [cmake]")

set(PRODUCT_NAME_SHORT "aVPN")
set(PRODUCT_NAME_LONG "aVPN")
set(PRODUCT_NAME_COMPANY "aVPN Team")
set(PRODUCT_NAME_COMPANY_DOMAIN "avpn.local")
set(PRODUCT_NAME_COPYRIGHT_SHORT "Copyright (C) 2016-2019 aVPN Team")
set(PRODUCT_NAME_COPYRIGHT_FULL "Copyright (C) 2014-2015 Red Hat Inc.<br>${PRODUCT_NAME_COPYRIGHT_SHORT}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake/Modules/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake/Includes/")

include(ProjectSettings)
include(ProjectDependencies)

if(MINGW)
    add_subdirectory(nsis)
endif()
add_subdirectory(src)

install(FILES LICENSE.txt
    DESTINATION .
    COMPONENT App
)

if(WIN32)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/node/windows/
        DESTINATION node
        COMPONENT SDWAN_Node
        FILES_MATCHING
        PATTERN "zerotier-one_x64.exe"
        PATTERN "zerotier-one.port"
        PATTERN "zttap300.cat"
        PATTERN "zttap300.inf"
        PATTERN "zttap300.sys"
        PATTERN "sdwan-start.bat"
        PATTERN "sdwan-network.bat"
    )
endif()

include(clang-format)
include(git_revision_main)
include(build_timestamp_main)

include(ProjectPackage)