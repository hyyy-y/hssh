# FindMbedTLS wrapper that recognises an mbedTLS target already created by
# FetchContent in the same configure run. Falls back to a normal search if the
# target is not present.

if(TARGET mbedcrypto)
    get_target_property(MBEDTLS_INCLUDE_DIR mbedcrypto INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT MBEDTLS_INCLUDE_DIR)
        get_target_property(MBEDTLS_INCLUDE_DIR mbedcrypto INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    endif()

    # Use the target name directly so CMake resolves it at generation time.
    set(MBEDTLS_CRYPTO_LIBRARY mbedcrypto)
    set(MBEDTLS_FOUND TRUE)

    message(STATUS "Found mbedTLS via FetchContent target: ${MBEDTLS_CRYPTO_LIBRARY}")
else()
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h)
    find_library(MBEDTLS_CRYPTO_LIBRARY NAMES mbedcrypto)

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(MbedTLS DEFAULT_MSG
        MBEDTLS_INCLUDE_DIR MBEDTLS_CRYPTO_LIBRARY)
endif()
