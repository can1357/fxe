# Networking — http (libcurl optional) + websocket client.
add_library(
    fxe_net
    STATIC
    src/net/cookie_jar.cpp
    src/net/http_client.cpp
    src/net/websocket_client.cpp
)
target_include_directories(fxe_net PUBLIC src include)
target_compile_features(fxe_net PUBLIC cxx_std_20)
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(fxe_net PUBLIC CURL::libcurl)
    target_compile_definitions(fxe_net PUBLIC FXE_HAS_CURL=1)
    target_link_libraries(fxe_os PUBLIC CURL::libcurl)
    target_compile_definitions(fxe_os PUBLIC FXE_HAS_CURL=1)
else()
    message(STATUS "libcurl not found; fetch() will reject")
endif()

if(FXE_ENABLE_NATIVE_TLS_HTTP2)
    find_package(MbedTLS CONFIG REQUIRED)
    find_package(PkgConfig REQUIRED)
    find_package(ZLIB REQUIRED)
    pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)
    target_link_libraries(
        fxe_net
        PRIVATE
            MbedTLS::mbedtls
            MbedTLS::mbedx509
            MbedTLS::mbedcrypto
            PkgConfig::NGHTTP2
            ZLIB::ZLIB
    )
    target_compile_definitions(fxe_net PUBLIC FXE_HAS_NATIVE_TLS_HTTP2_DEPS=1)
    target_sources(
        fxe_net
        PRIVATE
            src/net/tls_client.cpp
            src/net/tls_server.cpp
            src/net/http2_client.cpp
            src/net/http2_server.cpp
    )
else()
    target_compile_definitions(fxe_net PUBLIC FXE_HAS_NATIVE_TLS_HTTP2_DEPS=0)
endif()
if(WIN32)
    target_link_libraries(fxe_net PUBLIC ws2_32)
endif()
