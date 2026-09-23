find_package(Qt6 REQUIRED COMPONENTS Test)
if(NOT Qt6_FOUND)
    find_package(Qt5 REQUIRED COMPONENTS Test)
endif()

set(COMMON_SOURCES
    ../src/utils/Config.cpp
    ../src/utils/Crypto.cpp
    ../src/utils/Database.cpp
    ../src/core/SessionConfig.cpp
    ../src/core/SessionModel.cpp
    ../src/core/SessionRepository.cpp
    ../src/core/KeyStore.cpp
    ../src/app/dialogs/KeyManagerDialog.cpp
    ../src/app/dialogs/SessionLogViewer.cpp
    ../src/app/dialogs/RemoteSearchDialog.cpp
    ../src/app/dialogs/PermissionsDialog.cpp
    ../src/app/dialogs/ProcessDialog.cpp
    ../src/app/dialogs/DockerDialog.cpp
    ../src/app/dialogs/NetworkToolsDialog.cpp
    ../src/app/dialogs/ServerTransferDialog.cpp
    ../src/app/GuiHostKeyPrompt.cpp
    ../src/app/RemoteEditManager.cpp
    ../src/app/TransferRegistry.cpp
    ../src/app/widgets/SftpWidget.cpp
    ../src/app/widgets/CommandPalette.cpp
    ../src/app/widgets/TerminalOutlineWidget.cpp
    ../src/core/PortForward.cpp
    ../src/core/SftpSession.cpp
    ../src/core/ChannelCopySession.cpp
    ../src/core/RemoteCommandChannel.cpp
    ../src/app/ServerMonitor.cpp
    ../src/core/TransferSession.h
    ../src/core/SshConnect.cpp
    ../src/core/SshSession.cpp
    ../src/core/transport/TransportFactory.cpp
    ../src/core/ConnectionManager.cpp
    ../src/terminal/ShellProcess.cpp
    ../src/terminal/LocalShellProcess.cpp
    ../src/terminal/SshShellProcess.cpp
    ../src/terminal/TerminalSession.cpp
    ../src/terminal/SerialShellProcess.cpp
    ../src/terminal/ZModemEngine.cpp
    ../src/terminal/TerminalWidget.cpp
    ../src/utils/TextDiff.cpp
)

if(WIN32)
    list(APPEND COMMON_SOURCES ../src/terminal/LocalShellProcess_win.cpp)

    # LocalShellProcess prefers the bundled conpty.dll + OpenConsole.exe
    # (see third_party/conpty/README.md); place them next to the test exes.
    if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/conpty/conpty.dll)
        file(COPY
            ${CMAKE_SOURCE_DIR}/third_party/conpty/conpty.dll
            ${CMAKE_SOURCE_DIR}/third_party/conpty/OpenConsole.exe
            DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
        )
    endif()
else()
    list(APPEND COMMON_SOURCES ../src/terminal/LocalShellProcess_unix.cpp)
endif()

set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

if(libssh_FOUND)
    list(APPEND COMMON_INCLUDES
        ${libssh_SOURCE_DIR}/include
        ${libssh_BINARY_DIR}/include
    )
    if(DEFINED mbedtls_SOURCE_DIR)
        list(APPEND COMMON_INCLUDES ${mbedtls_SOURCE_DIR}/include)
    endif()
endif()
if(libvterm_FOUND)
    list(APPEND COMMON_INCLUDES ${VTERM_SOURCE_DIR}/include)
endif()

set(COMMON_LIBS
    Qt::Core
    Qt::Gui
    Qt::Widgets
    Qt::Network
    Qt::Test
    Qt::Sql
)

if(libvterm_FOUND)
    list(APPEND COMMON_LIBS vterm)
endif()
if(libssh_FOUND)
    list(APPEND COMMON_LIBS ssh)
endif()

if(UNIX AND NOT APPLE)
    include(CheckLibraryExists)
    check_library_exists(util forkpty "" HSSH_HAVE_LIBUTIL_FORKPTY)
    if(HSSH_HAVE_LIBUTIL_FORKPTY)
        list(APPEND COMMON_LIBS util)
    endif()
endif()

# Qt SQL plugins are needed for SQLite tests; the Qt6/runtime DLLs sit next
# to hssh.exe in the build root, so put it on PATH for the test exes.
# NB: every ';' inside the expanded PATH must be escaped, otherwise CMake
# splits it into separate (broken) ENVIRONMENT entries.
string(REPLACE ";" "\\;" HSSH_TEST_ENV_PATH "$ENV{PATH}")
set(TEST_ENV
    "QT_PLUGIN_PATH=${Qt6Sql_DIR}/../../../plugins"
    "PATH=${CMAKE_BINARY_DIR}\;${HSSH_TEST_ENV_PATH}"
)

add_executable(test_config test_config.cpp ${COMMON_SOURCES})
target_include_directories(test_config PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_config PRIVATE ${COMMON_LIBS})
add_test(NAME test_config COMMAND test_config)
set_tests_properties(test_config PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_keystore test_keystore.cpp ${COMMON_SOURCES})
target_include_directories(test_keystore PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_keystore PRIVATE ${COMMON_LIBS})
add_test(NAME test_keystore COMMAND test_keystore)
set_tests_properties(test_keystore PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_zmodem test_zmodem.cpp ${COMMON_SOURCES})
target_include_directories(test_zmodem PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_zmodem PRIVATE ${COMMON_LIBS})
add_test(NAME test_zmodem COMMAND test_zmodem)
set_tests_properties(test_zmodem PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_palette test_palette.cpp ${COMMON_SOURCES})
target_include_directories(test_palette PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_palette PRIVATE ${COMMON_LIBS})
add_test(NAME test_palette COMMAND test_palette)
set_tests_properties(test_palette PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_sessionlog test_sessionlog.cpp ${COMMON_SOURCES})
target_include_directories(test_sessionlog PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_sessionlog PRIVATE ${COMMON_LIBS})
add_test(NAME test_sessionlog COMMAND test_sessionlog)
set_tests_properties(test_sessionlog PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_remoteedit test_remoteedit.cpp ${COMMON_SOURCES})
target_include_directories(test_remoteedit PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_remoteedit PRIVATE ${COMMON_LIBS})
add_test(NAME test_remoteedit COMMAND test_remoteedit)
set_tests_properties(test_remoteedit PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_remotesearch test_remotesearch.cpp ${COMMON_SOURCES})
target_include_directories(test_remotesearch PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_remotesearch PRIVATE ${COMMON_LIBS})
add_test(NAME test_remotesearch COMMAND test_remotesearch)
set_tests_properties(test_remotesearch PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_permissions test_permissions.cpp ${COMMON_SOURCES})
target_include_directories(test_permissions PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_permissions PRIVATE ${COMMON_LIBS})
add_test(NAME test_permissions COMMAND test_permissions)
set_tests_properties(test_permissions PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_sysmon test_sysmon.cpp ${COMMON_SOURCES})
target_include_directories(test_sysmon PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_sysmon PRIVATE ${COMMON_LIBS})
add_test(NAME test_sysmon COMMAND test_sysmon)
set_tests_properties(test_sysmon PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_session_model test_session_model.cpp ${COMMON_SOURCES})
target_include_directories(test_session_model PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_session_model PRIVATE ${COMMON_LIBS})
add_test(NAME test_session_model COMMAND test_session_model)
set_tests_properties(test_session_model PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_session_repository test_session_repository.cpp ${COMMON_SOURCES})
target_include_directories(test_session_repository PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_session_repository PRIVATE ${COMMON_LIBS})
add_test(NAME test_session_repository COMMAND test_session_repository)
set_tests_properties(test_session_repository PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_connection_manager test_connection_manager.cpp ${COMMON_SOURCES})
target_include_directories(test_connection_manager PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_connection_manager PRIVATE ${COMMON_LIBS})
add_test(NAME test_connection_manager COMMAND test_connection_manager)
set_tests_properties(test_connection_manager PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_terminal test_terminal.cpp ${COMMON_SOURCES})
target_include_directories(test_terminal PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_terminal PRIVATE ${COMMON_LIBS})
add_test(NAME test_terminal COMMAND test_terminal)
set_tests_properties(test_terminal PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_sshsession test_sshsession.cpp ${COMMON_SOURCES})
target_include_directories(test_sshsession PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_sshsession PRIVATE ${COMMON_LIBS})
add_test(NAME test_sshsession COMMAND test_sshsession)
set_tests_properties(test_sshsession PROPERTIES ENVIRONMENT "${TEST_ENV}")

add_executable(test_textdiff test_textdiff.cpp ${COMMON_SOURCES})
target_include_directories(test_textdiff PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_textdiff PRIVATE ${COMMON_LIBS})
add_test(NAME test_textdiff COMMAND test_textdiff)
set_tests_properties(test_textdiff PROPERTIES ENVIRONMENT "${TEST_ENV}")

set(AGENT_SOURCES
    ../src/agent/AgentAudit.cpp
    ../src/agent/AgentHttpServer.cpp
    ../src/agent/AgentMcpServer.cpp
    ../src/agent/AgentPolicy.cpp
)

add_executable(test_agent_http test_agent_http.cpp ${COMMON_SOURCES} ${AGENT_SOURCES})
target_include_directories(test_agent_http PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_agent_http PRIVATE ${COMMON_LIBS})
add_test(NAME test_agent_http COMMAND test_agent_http)
set_tests_properties(test_agent_http PROPERTIES ENVIRONMENT "${TEST_ENV}")

if(libssh_FOUND)
    add_executable(test_sftp test_sftp.cpp ${COMMON_SOURCES})
    target_include_directories(test_sftp PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(test_sftp PRIVATE ${COMMON_LIBS})
    add_test(NAME test_sftp COMMAND test_sftp)
    set_tests_properties(test_sftp PROPERTIES ENVIRONMENT "${TEST_ENV}")

    add_executable(test_channel_copy test_channel_copy.cpp ${COMMON_SOURCES})
    target_include_directories(test_channel_copy PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(test_channel_copy PRIVATE ${COMMON_LIBS})
    add_test(NAME test_channel_copy COMMAND test_channel_copy)
    set_tests_properties(test_channel_copy PROPERTIES ENVIRONMENT "${TEST_ENV}")
endif()
