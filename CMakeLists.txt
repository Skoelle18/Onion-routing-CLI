cmake_minimum_required(VERSION 3.16)

project(onion VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
    set(ONION_WARNING_FLAGS /W4 /permissive-)
else()
    set(ONION_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

option(ONION_BUILD_TESTS "Build onion unit and integration tests" ON)

add_library(onion_core STATIC
    src/crypto/keys.cpp
    src/crypto/envelope.cpp
    src/onion/layer.cpp
    src/onion/builder.cpp
    src/onion/config.cpp
    src/net/address.cpp
    src/net/framing.cpp
    src/net/platform.cpp
    src/net/socket.cpp
)

target_include_directories(onion_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_options(onion_core PRIVATE ${ONION_WARNING_FLAGS})

target_link_libraries(onion_core
    PUBLIC OpenSSL::Crypto Threads::Threads
)

if(WIN32)
    target_compile_definitions(onion_core PUBLIC
        _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_link_libraries(onion_core PUBLIC ws2_32 bcrypt crypt32)
endif()

add_executable(onion
    src/cli/main.cpp
    src/cli/app.cpp
)

target_compile_options(onion PRIVATE ${ONION_WARNING_FLAGS})
target_link_libraries(onion PRIVATE onion_core)

install(TARGETS onion RUNTIME DESTINATION bin)

if(ONION_BUILD_TESTS)
    enable_testing()

    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_subdirectory(tests)
endif()
