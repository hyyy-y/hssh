cmake_minimum_required(VERSION 3.16)
project(hssh VERSION 0.1.0 LANGUAGES CXX C)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt specific
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6 packages
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Sql LinguistTools)

# Internationalization
set(HSSH_TRANSLATIONS
    resources/i18n/hssh_en.ts
    resources/i18n/hssh_zh_CN.ts
)

qt_add_translations(${PROJECT_NAME}
    TS_FILES ${HSSH_TRANSLATIONS}
    RESOURCE_PREFIX "/i18n"
)

# Optional SSH backend: libssh (preferred) or system OpenSSH executable fallback
find_package(libssh CONFIG QUIET)
if(libssh_FOUND)
    message(STATUS "libssh found, using native SSH backend")
    add_compile_definitions(HSSH_HAS_LIBSSH)
else()
    message(STATUS "libssh not found via find_package; fetching from source...")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchLibSSH.cmake)
if(Qt6_WebSockets_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt::WebSockets)
endif()

if(libssh_FOUND)
        message(STATUS "libssh fetched and built from source")
        add_compile_definitions(HSSH_HAS_LIBSSH)
    else()
        message(WARNING "libssh could not be fetched. SSH connections will use the system ssh executable fallback.")
    endif()
endif()

# Optional terminal backend: libvterm (preferred) or QPlainTextEdit fallback
find_package(libvterm CONFIG QUIET)
if(libvterm_FOUND)
    message(STATUS "libvterm found, using native terminal backend")
    add_compile_definitions(HSSH_HAS_LIBVTERM)
else()
    message(STATUS "libvterm not found via find_package; fetching from source...")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchLibVTerm.cmake)
    if(libvterm_FOUND)
        message(STATUS "libvterm fetched and built from source")
        add_compile_definitions(HSSH_HAS_LIBVTERM)
    else()
        message(WARNING "libvterm could not be fetched. Terminal will use a simplified QPlainTextEdit fallback.")
    endif()
endif()

# ---------------------------------------------------------------------------
# PH0-06: optional feature switches. Everything below degrades gracefully —
# a machine without the module/dependency still builds.
# ---------------------------------------------------------------------------

# GSSAPI/Kerberos authentication (PH2-11) is opt-in: it changes the bundled
# libssh build (full rebuild of _deps) and needs a domain environment to test.
option(HSSH_WITH_GSSAPI "Build the bundled libssh with GSSAPI/Kerberos (SSPI on Windows)" OFF)
if(HSSH_WITH_GSSAPI)
    add_compile_definitions(HSSH_HAS_GSSAPI)
    message(STATUS "GSSAPI enabled — libssh will be built with GSSAPI support")
endif()

# Qt6 WebSockets for the agent /ws endpoint (PH2-14); optional.
find_package(Qt6 QUIET COMPONENTS WebSockets)
if(Qt6_WebSockets_FOUND)
    message(STATUS "Qt6 WebSockets found — agent WebSocket endpoint available")
    add_compile_definitions(HSSH_HAS_WEBSOCKETS)
else()
    message(STATUS "Qt6 WebSockets not found — agent WebSocket endpoint disabled")
endif()

# System OpenSSH client (ControlMaster PH3-07, FIDO2 PH3-10 fallback paths).
find_program(HSSH_SSH_EXECUTABLE NAMES ssh ssh.exe)
if(HSSH_SSH_EXECUTABLE)
    string(REPLACE "\\" "/" HSSH_SSH_BIN_PATH "${HSSH_SSH_EXECUTABLE}")
    add_compile_definitions(HSSH_HAS_SSH_BIN HSSH_SSH_BIN="${HSSH_SSH_BIN_PATH}")
    message(STATUS "System ssh found: ${HSSH_SSH_EXECUTABLE}")
else()
    message(STATUS "System ssh not found — ControlMaster/FIDO2 fallbacks disabled")
endif()

# Source files
set(HSSH_SOURCES
    src/main.cpp
    src/agent/AgentAudit.cpp
    src/agent/AgentAudit.h
    src/agent/AgentHttpServer.cpp
    src/agent/AgentHttpServer.h
    src/agent/AgentMcpServer.cpp
    src/agent/AgentMcpServer.h
    src/agent/AgentPolicy.cpp
    src/agent/AgentPolicy.h
    src/agent/AgentSudoAuth.h
    src/agent/AgentWebSocketServer.cpp
    src/agent/AgentWebSocketServer.h
    src/app/InputBroadcaster.cpp
    src/app/InputBroadcaster.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/SessionTab.cpp
    src/app/SessionTab.h
    src/app/dialogs/AppearanceDialog.cpp
    src/app/dialogs/AppearanceDialog.h
    src/app/FloatingTabWindow.cpp
    src/app/FloatingTabWindow.h
    src/app/dialogs/KeyManagerDialog.cpp
    src/app/dialogs/KeyManagerDialog.h
    src/app/dialogs/KbdintPromptDialog.cpp
    src/app/dialogs/KbdintPromptDialog.h
    src/app/dialogs/EditFolderDialog.cpp
    src/app/dialogs/EditFolderDialog.h
    src/app/dialogs/DiffDialog.cpp
    src/app/dialogs/DiffDialog.h
    src/app/dialogs/ImportSshConfigDialog.cpp
    src/app/dialogs/ImportSshConfigDialog.h
    src/app/dialogs/NewSessionDialog.cpp
    src/app/dialogs/NewSessionDialog.h
    src/app/dialogs/PortForwardDialog.cpp
    src/app/dialogs/PortForwardDialog.h
    src/app/dialogs/SessionLogViewer.cpp
    src/app/dialogs/SessionLogViewer.h
    src/app/dialogs/RemoteSearchDialog.cpp
    src/app/dialogs/RemoteSearchDialog.h
    src/app/dialogs/PermissionsDialog.cpp
    src/app/dialogs/PermissionsDialog.h
    src/app/dialogs/ProcessDialog.cpp
    src/app/dialogs/ProcessDialog.h
    src/app/dialogs/DockerDialog.cpp
    src/app/dialogs/DockerDialog.h
    src/app/dialogs/NetworkToolsDialog.cpp
    src/app/dialogs/NetworkToolsDialog.h
    src/app/dialogs/SchedulerDialog.cpp
    src/app/dialogs/SchedulerDialog.h
    src/app/dialogs/ServerTransferDialog.cpp
    src/app/dialogs/ServerTransferDialog.h
    src/app/dialogs/SettingsDialog.cpp
    src/app/dialogs/SettingsDialog.h
    src/app/widgets/DiffView.cpp
    src/app/widgets/DiffView.h
    src/app/widgets/CommandPalette.cpp
    src/app/widgets/CommandPalette.h
    src/app/widgets/TerminalOutlineWidget.cpp
    src/app/widgets/TerminalOutlineWidget.h
    src/app/widgets/FileCompareWidget.cpp
    src/app/widgets/FileCompareWidget.h
    src/app/widgets/LocalFileWidget.cpp
    src/app/widgets/LocalFileWidget.h
    src/app/widgets/SessionManagerWidget.cpp
    src/app/widgets/SessionManagerWidget.h
    src/app/widgets/SftpWidget.cpp
    src/app/widgets/SftpWidget.h
    src/app/widgets/TransfersWidget.cpp
    src/app/widgets/TransfersWidget.h
    src/app/TransferRegistry.cpp
    src/app/TransferRegistry.h
    src/app/RemoteEditManager.cpp
    src/app/RemoteEditManager.h
    src/app/GuiHostKeyPrompt.cpp
    src/app/GuiHostKeyPrompt.h
    src/core/ConnectionManager.cpp
    src/core/ConnectionManager.h
    src/core/KeyStore.cpp
    src/core/KeyStore.h
    src/core/PortForward.cpp
    src/core/PortForward.h
    src/core/SessionConfig.cpp
    src/core/SessionConfig.h
    src/core/SessionModel.cpp
    src/core/SessionModel.h
    src/core/SessionRepository.cpp
    src/core/SessionRepository.h
    src/core/SftpSession.cpp
    src/core/SftpSession.h
    src/core/ChannelCopySession.cpp
    src/core/ChannelCopySession.h
    src/core/RemoteCommandChannel.cpp
    src/core/RemoteCommandChannel.h
    src/app/ServerMonitor.cpp
    src/app/ServerMonitor.h
    src/app/widgets/MonitorWidget.cpp
    src/app/widgets/MonitorWidget.h
    src/core/TransferSession.h
    src/core/SshConnect.cpp
    src/core/SshConnect.h
    src/core/SshSession.cpp
    src/core/SshSession.h
    src/core/transport/ITransport.h
    src/core/transport/TransportFactory.cpp
    src/core/transport/TransportFactory.h
    src/terminal/LocalShellProcess.cpp
    src/terminal/LocalShellProcess.h
    src/terminal/SerialShellProcess.cpp
    src/terminal/SerialShellProcess.h
    src/terminal/ShellProcess.cpp
    src/terminal/ShellProcess.h
    src/terminal/SshShellProcess.cpp
    src/terminal/SshShellProcess.h
    src/terminal/TerminalSession.cpp
    src/terminal/TerminalSession.h
    src/terminal/TerminalWidget.cpp
    src/terminal/TerminalWidget.h
    src/terminal/ZModemEngine.cpp
    src/terminal/ZModemEngine.h
    src/utils/Config.cpp
    src/utils/Config.h
    src/utils/Crypto.cpp
    src/utils/Crypto.h
    src/utils/Database.cpp
    src/utils/Database.h
    src/utils/Logger.cpp
    src/utils/Logger.h
    src/utils/TextDiff.cpp
    src/utils/TextDiff.h
    src/utils/Theme.cpp
    src/utils/Theme.h
    resources/resources.qrc
    resources/hssh.rc
)

# Platform-specific local shell backend sources.
if(WIN32)
    list(APPEND HSSH_SOURCES src/terminal/LocalShellProcess_win.cpp)
else()
    list(APPEND HSSH_SOURCES src/terminal/LocalShellProcess_unix.cpp)
endif()

# Executable
add_executable(${PROJECT_NAME} ${HSSH_SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt::Core
    Qt::Widgets
    Qt::Network
    Qt::Sql
)

if(libssh_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${libssh_SOURCE_DIR}/include
        ${libssh_BINARY_DIR}/include
    )
    if(DEFINED mbedtls_SOURCE_DIR)
        target_include_directories(${PROJECT_NAME} PRIVATE ${mbedtls_SOURCE_DIR}/include)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE ssh)
endif()

if(libvterm_FOUND)
    if(TARGET vterm)
        target_link_libraries(${PROJECT_NAME} PRIVATE vterm)
    elseif(DEFINED libvterm_TARGET)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${libvterm_TARGET})
    endif()
endif()

# Platform specific settings
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    # Bundled Microsoft Terminal conhost (see third_party/conpty/README.md):
    # the inbox conhost on Windows 11 26100+ kills pseudoconsole clients, so
    # LocalShellProcess prefers conpty.dll + OpenConsole.exe when they sit
    # next to the executable.
    set(HSSH_CONPTY_DIR ${CMAKE_SOURCE_DIR}/third_party/conpty)
    if(EXISTS ${HSSH_CONPTY_DIR}/conpty.dll)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${HSSH_CONPTY_DIR}/conpty.dll
                ${HSSH_CONPTY_DIR}/OpenConsole.exe
                $<TARGET_FILE_DIR:${PROJECT_NAME}>
        )
        install(FILES
            ${HSSH_CONPTY_DIR}/conpty.dll
            ${HSSH_CONPTY_DIR}/OpenConsole.exe
            DESTINATION bin
        )
    endif()
elseif(UNIX AND NOT APPLE)
    # forkpty/openpty may live in libutil on glibc < 2.34.
    include(CheckLibraryExists)
    check_library_exists(util forkpty "" HSSH_HAVE_LIBUTIL_FORKPTY)
    if(HSSH_HAVE_LIBUTIL_FORKPTY)
        target_link_libraries(${PROJECT_NAME} PRIVATE util)
    endif()
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive-)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Install rules
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

# One-click portable package (Windows): `cmake --build . --target portable`
# Collects hssh.exe + Qt runtime + plugins + bundled conpty into dist/ and
# zips it as hssh-portable-win64.zip next to the build directory.
if(WIN32)
    set(HSSH_DIST_DIR ${CMAKE_BINARY_DIR}/dist)
    find_program(HSSH_WINDEPLOYQT_EXE windeployqt
        HINTS ${Qt6_DIR}/../../../bin ${Qt6_DIR}/../../bin)
    if(HSSH_WINDEPLOYQT_EXE)
        add_custom_target(portable
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${HSSH_DIST_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${HSSH_DIST_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${PROJECT_NAME}> ${HSSH_DIST_DIR}/
            COMMAND ${HSSH_WINDEPLOYQT_EXE} --release --no-translations
                --compiler-runtime --dir ${HSSH_DIST_DIR}
                $<TARGET_FILE:${PROJECT_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/LICENSE
                ${CMAKE_SOURCE_DIR}/README.md
                ${CMAKE_SOURCE_DIR}/docs/agent.md
                ${HSSH_DIST_DIR}/
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/third_party/conpty/conpty.dll
                ${CMAKE_SOURCE_DIR}/third_party/conpty/OpenConsole.exe
                ${HSSH_DIST_DIR}/
            # Archive with cmake -E tar (PowerShell's Compress-Archive needs a
            # working .NET type system, which is not a given on every machine).
            # -E chdir instead of WORKING_DIRECTORY: the rm -rf above must not
            # run with its own target directory as the current directory.
            COMMAND ${CMAKE_COMMAND} -E chdir ${HSSH_DIST_DIR}
                ${CMAKE_COMMAND} -E tar cf
                ${CMAKE_BINARY_DIR}/hssh-portable-win64.zip --format=zip .
            DEPENDS ${PROJECT_NAME}
            COMMENT "Packaging portable build into dist/ and hssh-portable-win64.zip"
        )
    else()
        message(WARNING "windeployqt not found; 'portable' target unavailable")
    endif()
endif()

# Tests
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
