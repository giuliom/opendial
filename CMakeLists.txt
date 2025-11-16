cmake_minimum_required(VERSION 3.23)
project(OpenDialProject LANGUAGES CXX)

set(LIBRARY_NAME OpenDialLib)
set(TEST_NAME OpenDialTest)

# Enable folders in IDEs (optional)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# GoogleTest requires at least C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Avoid warnings by using new policy
cmake_policy(SET CMP0135 NEW)

include(FetchContent)

# ------ Fetch Google Test ------
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ------ Fetch standalone Asio ------
# Official repository: https://github.com/chriskohlhoff/asio
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-34-2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(asio)

# Create an interface library for asio
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ${ASIO_DEFS})

# Ensure pthread is used on Unix-like systems
if(UNIX AND NOT APPLE)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
elseif(APPLE)
  find_package(Threads REQUIRED)
endif()

# Helper to link system libs needed by Asio (sockets, threads)
function(link_platform_libs target)
  if(WIN32)
    target_link_libraries(${target} PRIVATE ws2_32) # Winsock
  else()
    target_link_libraries(${target} PRIVATE Threads::Threads)
  endif()
endfunction()

# Shared definitions for standalone Asio
set(ASIO_DEFS
  ASIO_STANDALONE            # use standalone (no Boost)
  ASIO_NO_DEPRECATED         # optional: hide deprecated API
)


# ------ Argvnaut Library ------
file(GLOB LIBRARY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(${LIBRARY_NAME} ${LIBRARY_SOURCES})
target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ------ Test Target ------
file (GLOB TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")
add_executable(${TEST_NAME} ${TESTS}) 

target_link_libraries(
    ${TEST_NAME}
    GTest::gtest_main
    ${LIBRARY_NAME}
  )

  target_include_directories(${TEST_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/")

  include(GoogleTest)
  gtest_discover_tests(${TEST_NAME})