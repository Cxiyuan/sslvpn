
set_property(SOURCE ${PROJECT_NAME}.cpp PROPERTY SKIP_AUTOGEN ON)
set_property(SOURCE timestamp.cpp PROPERTY SKIP_AUTOGEN ON)

set(${PROJECT_NAME}_SRCS
    ${PROJECT_NAME}.cpp
    ${PROJECT_NAME}.qrc
    cert.cpp
    config.h.in
    cryptdata.cpp
    dialog/MyCertMsgBox.cpp
    dialog/MyInputDialog.cpp
    dialog/MyMsgBox.cpp
    dialog/editdialog.cpp
    dialog/editdialog.ui
    dialog/NewProfileDialog.cpp
    dialog/NewProfileDialog.ui
    dialog/logdialog.cpp
    dialog/logdialog.ui
    dialog/VpnProtocolModel.cpp
    dialog/mainwindow.cpp
    dialog/mainwindow.ui
    gtdb.cpp
    key.cpp
    keypair.cpp
    main.cpp
    server_storage.cpp

    timestamp.cpp

    vpninfo.cpp

    logger.cpp

    FileLogger.cpp
)

set_source_files_properties(
    ${PROJECT_NAME}.cpp
    timestamp.cpp
    PROPERTIES
        GENERATED true
)

configure_file(
    config.h.in
    config.h
    @ONLY
)

if(MINGW)
    list(APPEND ${PROJECT_NAME}_RESOURCES
        ${PROJECT_NAME}.rc.in
        ${PROJECT_NAME}.rc
    )

    set_source_files_properties(
       ${PROJECT_NAME}.rc
        PROPERTIES
            GENERATED true
    )
endif()
if(APPLE)
    if(EXISTS ${OPENCONNECT_VPNC_SCRIPT})
        list(APPEND ${PROJECT_NAME}_RESOURCES
            Resources/mono_lock.icns
            ${OPENCONNECT_VPNC_SCRIPT}
        )
        set_source_files_properties(
            Resources/mono_lock.icns
            ${OPENCONNECT_VPNC_SCRIPT}
                PROPERTIES MACOSX_PACKAGE_LOCATION Resources
        )
    else()
        list(APPEND ${PROJECT_NAME}_RESOURCES
            Resources/mono_lock.icns
        )
        set_source_files_properties(
            Resources/mono_lock.icns
                PROPERTIES MACOSX_PACKAGE_LOCATION Resources
        )
    endif()
endif()

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SRCS}
    ${${PROJECT_NAME}_RESOURCES}
)
if(MINGW)
    add_definitions(-DSPDLOG_WCHAR_FILENAMES)

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/external/vpnc-script.js $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "Deploying vpnc script..."
        VERBATIM
    )

    add_dependencies(${PROJECT_NAME}
        openconnect-devel-${openconnect-TAG}
        openconnect-${openconnect-TAG}
        vpnc-scripts-${vpnc-scripts-TAG}
    )

    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE true
    )

    target_link_libraries (${PROJECT_NAME}
        # system libraries
        wsock32

        # 3rd party dependencies
        openconnect::gmp
        openconnect::gnutls
        openconnect::hogweed
        openconnect::nettle
        openconnect::openconnect
        openconnect::p11-kit
        openconnect::stoken
        openconnect::xml2
    )
endif()

if(APPLE)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Resources/${PROJECT_NAME}.plist.in
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.plist
        @ONLY
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE true
        OUTPUT_NAME ${PRODUCT_NAME_SHORT}
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.plist"
        MACOSX_BUNDLE_BUNDLE_NAME ${PRODUCT_NAME_SHORT}
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
        MACOSX_BUNDLE_COPYRIGHT "${PRODUCT_NAME_COPYRIGHT_SHORT}"
        MACOSX_BUNDLE_GUI_IDENTIFIER com.avpn.avpn
        MACOSX_BUNDLE_ICON_FILE mono_lock.icns
        MACOSX_BUNDLE_INFO_STRING ${PRODUCT_NAME_LONG}
    )
endif()


# common stuff to link...
target_link_libraries (${PROJECT_NAME}
    spdlog::spdlog
    qt-solutions::qtsingleapplication

    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
    #??? Qt5::QMacStylePlugin
)

if(UNIX)
    target_link_libraries(${PROJECT_NAME}
        ${OPENCONNECT_LIBRARIES}
        ${GNUTLS_LIBRARY}

        Threads::Threads
    )
    if(APPLE)
        target_link_libraries (${PROJECT_NAME}
            ${SECURITY_LIBRARY}
        )
    endif()
endif()

# install executable
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION . COMPONENT App
    BUNDLE DESTINATION . COMPONENT App
    LIBRARY DESTINATION . COMPONENT App
)

# Install all DLLs from bin directory to package
if(MINGW)
    install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/
        DESTINATION .
        COMPONENT App
        FILES_MATCHING PATTERN "*.dll"
    )
    install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins/
        DESTINATION plugins
        COMPONENT App
        OPTIONAL
    )
endif()

# fixup the bundle
get_target_property(QT_LIBRARY_DIR Qt5::Core LOCATION)
get_filename_component(QT_LIBRARY_DIR ${QT_LIBRARY_DIR} PATH)
get_filename_component(QT_LIBRARY_DIR "${QT_LIBRARY_DIR}/.." ABSOLUTE)
list(APPEND libSearchDirs ${QT_LIBRARY_DIR})

if(MINGW)
    set(APPS "\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.exe")
    #set(APPS "\${CMAKE_INSTALL_PREFIX}/./${PROJECT_NAME}.exe")
    list(APPEND libSearchDirs ${CMAKE_BINARY_DIR}/external/lib)

    # Qt Platform Plugin
    install(FILES
        ${_qwindows_dll}
        DESTINATION plugins/platforms
        COMPONENT App
    )
    install(FILES
        ${CMAKE_SOURCE_DIR}/nsis/qt.conf
        DESTINATION .
        COMPONENT App
    )

    set(additionalLib ${_qwindows_dll})
    if(NOT Qt5Core_VERSION STRLESS "5.10")
        install(FILES
            ${_qwinstyle_dylib}
            DESTINATION plugins/styles
            COMPONENT App
        )
    endif()
elseif(APPLE)
    set(APPS "\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app")
    list(APPEND libSearchDirs "")

    # Qt Platform Plugin
    install(FILES
        ${_qcocoa_dylib}
        DESTINATION ${PROJECT_NAME}.app/Contents/PlugIns/platforms
        COMPONENT App
    )
    set(additionalLib \${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app/Contents/PlugIns/platforms/libqcocoa.dylib)
    if(NOT Qt5Core_VERSION STRLESS "5.10")
        install(FILES
            ${_qmacstyle_dylib}
            DESTINATION ${PROJECT_NAME}.app/Contents/PlugIns/styles
            COMPONENT App
        )
        set(additionalLib
            ${additionalLib}
            \${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app/Contents/PlugIns/styles/libqmacstyle.dylib
        )
    endif()
    install(FILES
        ${CMAKE_SOURCE_DIR}/bundle/qt.conf
        DESTINATION ${PROJECT_NAME}.app/Contents/Resources
        COMPONENT App
    )
    
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/node/macos/
        DESTINATION ${PROJECT_NAME}.app/Contents/Resources/node
        COMPONENT App
        FILES_MATCHING
        PATTERN "zerotier-one"
        PATTERN "MacEthernetTapAgent"
        PATTERN "zerotier-one.port"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
else()
    message(STATUS "Nothing to bundle...")
endif()

install(CODE "
    include(BundleUtilities)
    if(APPLE)
        set(BU_CHMOD_BUNDLE_ITEMS on)
        fixup_bundle(\"${APPS}\" \"${additionalLib}\" \"${libSearchDirs}\")
    endif()
    " DESTINATION . COMPONENT App
)