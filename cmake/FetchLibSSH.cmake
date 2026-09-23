# Fetch and build libssh from source with mbedTLS as the crypto backend.
# This is used as a fallback when find_package(libssh) fails.

include(FetchContent)

# Ensure our custom FindMbedTLS.cmake is on the module path when libssh runs
# find_package(MbedTLS).
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})

# ---------------------------------------------------------------------------
# mbedTLS (crypto backend for libssh)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        v2.28.8
)

set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(MBEDTLS_CONFIG_FILE "" CACHE STRING "" FORCE)

message(STATUS "Fetching mbedTLS for libssh crypto backend (via proxy if configured)...")
FetchContent_MakeAvailable(mbedtls)

# mbedTLS 2.28 uses the legacy CryptoAPI for entropy on Windows. On Windows 11
# the required CSP is frequently unavailable, so patch it to use CNG
# (BCryptGenRandom) instead.
file(READ ${mbedtls_SOURCE_DIR}/library/entropy_poll.c _entropy_poll)
string(REPLACE
    "#include <windows.h>\n#include <wincrypt.h>\n\nint mbedtls_platform_entropy_poll(void *data, unsigned char *output, size_t len,\n                                  size_t *olen)\n{\n    HCRYPTPROV provider;\n    ((void) data);\n    *olen = 0;\n\n    if (CryptAcquireContext(&provider, NULL, NULL,\n                            PROV_RSA_AES, CRYPT_VERIFYCONTEXT) == FALSE) {\n        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;\n    }\n\n    if (CryptGenRandom(provider, (DWORD) len, output) == FALSE) {\n        CryptReleaseContext(provider, 0);\n        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;\n    }\n\n    CryptReleaseContext(provider, 0);\n    *olen = len;\n\n    return 0;\n}"
    "#include <windows.h>\n#include <bcrypt.h>\n\nint mbedtls_platform_entropy_poll(void *data, unsigned char *output, size_t len,\n                                  size_t *olen)\n{\n    ((void) data);\n    *olen = 0;\n\n    if (BCryptGenRandom(NULL, output, (ULONG) len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {\n        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;\n    }\n\n    *olen = len;\n    return 0;\n}"
    _entropy_poll "${_entropy_poll}")
file(WRITE ${mbedtls_SOURCE_DIR}/library/entropy_poll.c "${_entropy_poll}")

# mbedTLS 2.28 on Windows needs additional system libraries for entropy.
if(WIN32)
    target_link_libraries(mbedcrypto PUBLIC ws2_32 advapi32 bcrypt)
endif()

# libssh's pthread threading backend requires mbedtls to be built with threading
# support enabled (either MBEDTLS_THREADING_PTHREAD or MBEDTLS_THREADING_ALT).
# Enable pthread threading so crypto_thread_init() succeeds during ssh_init().
target_compile_definitions(mbedcrypto PUBLIC MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD)
target_compile_definitions(mbedtls PUBLIC MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD)
target_compile_definitions(mbedx509 PUBLIC MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD)
if(WIN32)
    target_link_libraries(mbedcrypto PUBLIC winpthread)
else()
    target_link_libraries(mbedcrypto PUBLIC pthread)
endif()

# ---------------------------------------------------------------------------
# libssh
# ---------------------------------------------------------------------------
FetchContent_Declare(
    libssh
    GIT_REPOSITORY https://gitlab.com/libssh/libssh-mirror.git
    GIT_TAG        libssh-0.10.6
)

set(WITH_MBEDTLS ON CACHE BOOL "" FORCE)
set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)
set(WITH_GCRYPT OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
# PH0-06: GSSAPI/Kerberos is opt-in (HSSH_WITH_GSSAPI). On Windows libssh
# uses SSPI (secur32); on Linux it needs krb5 development packages.
if(NOT DEFINED HSSH_WITH_GSSAPI)
    set(HSSH_WITH_GSSAPI OFF)
endif()
set(WITH_GSSAPI ${HSSH_WITH_GSSAPI} CACHE BOOL "" FORCE)
set(WITH_NACL OFF CACHE BOOL "" FORCE)
set(WITH_SERVER OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_STATIC_LIB ON CACHE BOOL "" FORCE)
set(WITH_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_DEBUG_CALLTRACE OFF CACHE BOOL "" FORCE)
set(WITH_DEBUG_CRYPTO OFF CACHE BOOL "" FORCE)
set(WITH_INTERNAL_DOC OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING OFF CACHE BOOL "" FORCE)

message(STATUS "Fetching libssh from upstream (via proxy if configured)...")
FetchContent_Populate(libssh)

# libssh tries to create a source-directory symlink for compile_commands.json,
# which fails on Windows without developer mode / admin rights. Patch it out.
file(READ ${libssh_SOURCE_DIR}/CMakeLists.txt _libssh_cmake)
string(REPLACE
    "execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink\n                \"\${CMAKE_BINARY_DIR}/compile_commands.json\"\n                \"\${CMAKE_SOURCE_DIR}/compile_commands.json\")"
    "# compile_commands.json symlink removed for Windows compatibility"
    _libssh_cmake "${_libssh_cmake}")
file(WRITE ${libssh_SOURCE_DIR}/CMakeLists.txt "${_libssh_cmake}")

file(READ ${libssh_SOURCE_DIR}/cmake/Modules/DefineCMakeDefaults.cmake _libssh_defaults)
string(REPLACE
    "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)"
    "set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)"
    _libssh_defaults "${_libssh_defaults}")
file(WRITE ${libssh_SOURCE_DIR}/cmake/Modules/DefineCMakeDefaults.cmake "${_libssh_defaults}")

# libssh also tries to install an export set that references the external
# mbedcrypto target. Since we only static-link libssh into hssh, disable the
# export installation.
file(READ ${libssh_SOURCE_DIR}/src/CMakeLists.txt _libssh_src_cmake)
string(REPLACE
    "install(EXPORT libssh-config\n        DESTINATION \${CMAKE_INSTALL_LIBDIR}/cmake/\${PROJECT_NAME})"
    "# install(EXPORT libssh-config disabled for FetchContent build"
    _libssh_src_cmake "${_libssh_src_cmake}")
file(WRITE ${libssh_SOURCE_DIR}/src/CMakeLists.txt "${_libssh_src_cmake}")

add_subdirectory(${libssh_SOURCE_DIR} ${libssh_BINARY_DIR})

set(libssh_FOUND TRUE)
set(libssh_TARGET ssh)
