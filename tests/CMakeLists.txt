cmake_minimum_required(VERSION 3.16)

project(simtemp_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(simtemp_tests
    simtemp_gtest.cpp
)

target_link_libraries(simtemp_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        pthread
)
