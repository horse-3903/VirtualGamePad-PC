cmake_minimum_required(VERSION 3.19)

set(APP_VERSION 0.6.0)
add_definitions(-DAPP_VERSION="${APP_VERSION}")
if(WIN32)
    add_definitions(-DNOMINMAX)
endif()

project("AirDeck" VERSION ${APP_VERSION} LANGUAGES CXX C)
if(WIN32)
    enable_language(RC)
    list(APPEND PROJECT_SOURCES_WIN res/AirDeck.rc)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Platform validation
if (NOT WIN32 AND NOT LINUX)
    message(FATAL_ERROR "This project only supports Windows and Linux")
endif()

# Add clang-format target for code formatting
find_program(CLANG_FORMAT "clang-format")
if(CLANG_FORMAT)
    # Get all source files
    file(GLOB_RECURSE ALL_SOURCE_FILES 
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_SOURCE_DIR}/src/*.h"
    )
    
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -style=file -i ${ALL_SOURCE_FILES}
        COMMENT "Formatting source code with clang-format"
        VERBATIM
    )
    
    message(STATUS "clang-format found: ${CLANG_FORMAT}")
    message(STATUS "Format target added. Use 'cmake --build build --target format' to format the code.")
else()
    message(STATUS "clang-format not found. Format target not created.")
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Svg)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Platform-specific setup
if(WIN32)
    # Include CppWinRT setup for Windows
    include(CppWinRT)
elseif(LINUX)
    # Include Linux input dependencies
    include(LinuxInput)
endif()

add_subdirectory(VGP_Data_Exchange/C)

set(QR_CODE_GEN_SOURCES
    third-party-libs/QR-Code-generator/cpp/qrcodegen.cpp
    third-party-libs/QR-Code-generator/cpp/qrcodegen.hpp
)

if(WIN32)
    add_subdirectory(third-party-libs/ViGEmClient EXCLUDE_FROM_ALL)
endif()

add_library(QR_Code_Generator SHARED ${QR_CODE_GEN_SOURCES})
set_target_properties(QR_Code_Generator PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Manifest file for the application
# This file is used to request permissions on Windows
SET(APP_MANIFEST_FILE "${CMAKE_SOURCE_DIR}/res/AirDeck.exe.manifest")

# Add project files sorted in alphabetical order
set(PROJECT_SOURCES
    res/icons.qrc
    res/web.qrc
    src/appdir.hpp
    src/main.cpp
    src/networking/executor.cpp
    src/networking/executor.hpp
    src/networking/server.cpp
    src/networking/server.hpp
    src/networking/server.ui
    src/settings/settings.hpp
    src/settings/settings_singleton.cpp
    src/settings/settings_singleton.hpp
    src/settings/keymap_profile.hpp
    src/settings/keymap_profile.cpp
    src/simulation/gamepadSim.hpp
    src/simulation/keyboardSim.hpp
    src/simulation/mouseSim.hpp
    src/ui/about.cpp
    src/ui/about.hpp
    src/ui/about.ui
    src/ui/badge.cpp
    src/ui/badge.hpp
    src/ui/buttoninputbox.cpp
    src/ui/controllertester.cpp
    src/ui/toastpopup.cpp
    src/ui/glyphs.cpp
    src/ui/glyphs.hpp
    src/platform/toast.cpp
    src/platform/toast.hpp
    src/platform/padcleanup.cpp
    src/platform/padcleanup.hpp
    src/platform/adapterinfo.cpp
    src/platform/adapterinfo.hpp
    src/debuglog.cpp
    src/debuglog.hpp
    src/ui/toastpopup.hpp
    src/ui/controllertester.hpp
    src/ui/slotcolors.hpp
    src/ui/buttoninputbox.hpp
    src/ui/mainmenu.cpp
    src/ui/mainmenu.hpp
    src/ui/mainmenu.ui
    src/ui/mainwindow.cpp
    src/ui/mainwindow.hpp
    src/ui/mainwindow.ui
    src/ui/preferences.cpp
    src/ui/preferences.hpp
    src/ui/preferences.ui
    src/ui/shotrunner.cpp
    src/ui/shotrunner.hpp
)

# Platform-specific simulation sources
if(WIN32)
    list(APPEND PROJECT_SOURCES
        src/platform/windows/console.cpp
        src/platform/windows/console.hpp
        src/simulation/windows/gamepadSim.cpp
        src/simulation/windows/keyboardSim.cpp
        src/simulation/windows/mouseSim.cpp
    )
elseif(LINUX)
    list(APPEND PROJECT_SOURCES
        src/simulation/linux/gamepadSim.cpp
        src/simulation/linux/keyboardSim.cpp
        src/simulation/linux/mouseSim.cpp
    )
endif()

qt_add_executable(AirDeck
    ${PROJECT_SOURCES}
    ${PROJECT_SOURCES_WIN}
)

target_link_libraries(AirDeck PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Svg
    Data_Exchange
    QR_Code_Generator
)

# Platform-specific linking and include directories
if(WIN32)
    target_link_libraries(AirDeck PRIVATE
        cppwinrt
        WindowsApp
        ViGEmClient
        setupapi
        cfgmgr32
        psapi
        shell32
        iphlpapi
        advapi32
        ws2_32
    )
elseif(LINUX)
    target_link_libraries(AirDeck PRIVATE
        ${UINPUT_LIBRARIES}
    )
    target_include_directories(AirDeck PRIVATE
        ${UINPUT_INCLUDE_DIRS}
    )
endif()

set_target_properties(AirDeck PROPERTIES
    WIN32_EXECUTABLE TRUE
)

set(CMAKE_INSTALL_DIR "${CMAKE_SOURCE_DIR}/dist")

install(TARGETS AirDeck Data_Exchange QR_Code_Generator
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Platform-specific resources/tools
if(WIN32)
    # Manifest file for the application
    # This file is used to request permissions on Windows
    SET(APP_MANIFEST_FILE "${CMAKE_SOURCE_DIR}/res/AirDeck.exe.manifest")

    # Windows deployment tool
    if(NOT DEFINED WINDEPLOYQT_EXECUTABLE)
        # Find the windeployqt executable in the Qt installation path
        # This is typically located in the bin directory of the Qt installation
        # Adjust the path based on your Qt installation
        find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS ${CMAKE_PREFIX_PATH}/bin)
    endif()
    if(WINDEPLOYQT_EXECUTABLE)
        message(STATUS "Found windeployqt: ${WINDEPLOYQT_EXECUTABLE}")
    endif()
endif()

# Platform-specific installation
if(WIN32)
    install(FILES "${APP_MANIFEST_FILE}" 
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )

    install(
        CODE "execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/AirDeck.exe\")"
    )
elseif(LINUX)
    # https://cmake.org/cmake/help/latest/prop_tgt/INSTALL_RPATH.html
    set_target_properties(AirDeck PROPERTIES
        INSTALL_RPATH_USE_LINK_PATH TRUE
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH FALSE
    )
    qt_generate_deploy_app_script(
        TARGET AirDeck
        OUTPUT_SCRIPT deploy_script
    )
    message(STATUS "Deploy script generated: ${deploy_script}")
    install(SCRIPT ${deploy_script})
endif()

# Platform-specific post-build steps
if(WIN32)
    # On Windows, we need to copy the DLLs to the output directory using windeployqt
    # This is done by adding a custom command as a post-build step
    # The CMake Cache variable WINDEPLOYQT_EXECUTABLE should contain the path to windeployqt
    add_custom_command(
        TARGET AirDeck POST_BUILD
        COMMAND ${WINDEPLOYQT_EXECUTABLE} $<TARGET_FILE:AirDeck>
        COMMENT "Running windeployqt to copy Qt dependencies to output directory"
        VERBATIM
    )

    # Configure the executable to use the manifest
    # Just place the manifest in the output directory and Windows will use it
    add_custom_command(
        TARGET AirDeck POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${APP_MANIFEST_FILE}
            $<TARGET_FILE_DIR:AirDeck>/AirDeck.exe.manifest
        COMMENT "Copying manifest file to output directory"
        VERBATIM
    )
endif()
