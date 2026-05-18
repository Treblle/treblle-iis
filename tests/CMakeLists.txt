cmake_minimum_required(VERSION 3.20)
project(TreblleModuleTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(TreblleTests
  DataMaskerTests.cpp
  UtilsTests.cpp
  ConfigTests.cpp
  AsyncQueueTests.cpp
  BodyCaptureTests.cpp
  PayloadBuilderTests.cpp

  ${SRC}/DataMasker.cpp
  ${SRC}/Utils.cpp
  ${SRC}/Config.cpp
  ${SRC}/AsyncQueue.cpp
  ${SRC}/BodyCapture.cpp
  ${SRC}/PayloadBuilder.cpp
)

target_include_directories(TreblleTests PRIVATE
  ${SRC}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Use the same precompiled header as the main project.
# This gives all source files the Windows/IIS SDK includes without
# having to list them individually.
target_precompile_headers(TreblleTests PRIVATE ${SRC}/precomp.h)

target_link_libraries(TreblleTests
  GTest::gtest_main
  winhttp
  ws2_32
  iphlpapi
)

include(GoogleTest)
gtest_discover_tests(TreblleTests)
