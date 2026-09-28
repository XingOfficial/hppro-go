cmake_minimum_required(VERSION 3.22.1)
project("hpcore" CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QUIC_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/${ANDROID_ABI})
if(EXISTS ${QUIC_LIB_DIR}/libngtcp2.a)
    set(HAVE_QUIC ON)
endif()

add_library(hpcore SHARED
    hp_jni.cpp
    hp_protocol.cpp
    hp_client.cpp)

target_include_directories(hpcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/prebuilt)

if(HAVE_QUIC)
    add_library(hpcore_quic SHARED quic_client.cpp quic_jni.cpp hp_protocol.cpp)
    target_include_directories(hpcore_quic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/prebuilt)
    target_link_libraries(hpcore_quic
        ${QUIC_LIB_DIR}/libngtcp2.a
        ${QUIC_LIB_DIR}/libngtcp2_crypto_ossl.a
        ${QUIC_LIB_DIR}/libnghttp3.a
        ${QUIC_LIB_DIR}/libssl.a
        ${QUIC_LIB_DIR}/libcrypto.a
        android log)
endif()

find_library(log-lib log)
target_link_libraries(hpcore android ${log-lib})
