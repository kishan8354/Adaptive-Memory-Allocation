cmake_minimum_required(VERSION 3.14)
project(AdvancedAllocators VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -D_DEBUG -fsanitize=address,undefined")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

# Warnings
add_compile_options(-Wall -Wextra -Wpedantic)

include_directories(include)

# Collect all sources
set(ALLOCATOR_SOURCES
    src/CAllocator.cpp
    src/LinearAllocator.cpp
    src/StackAllocator.cpp
    src/PoolAllocator.cpp
    src/FreeListAllocator.cpp
    src/BuddyAllocator.cpp
    src/SlabAllocator.cpp
    src/TLSFAllocator.cpp
    src/AdaptiveAllocator.cpp
    src/Benchmark.cpp
)

# Main executable
add_executable(allocator_demo ${ALLOCATOR_SOURCES} main.cpp)
target_include_directories(allocator_demo PRIVATE include)

# Optional: if you have Google Test installed
# find_package(GTest QUIET)
# if(GTest_FOUND)
#   add_executable(allocator_tests ${ALLOCATOR_SOURCES} tests/test_allocators.cpp)
#   target_link_libraries(allocator_tests GTest::GTest GTest::Main)
# endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
