cmake_minimum_required(VERSION 3.25)

project(hashsets
    VERSION 1.0
    DESCRIPTION "Implementations of concurrent hash sets"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")

find_package(Threads REQUIRED)

include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_ENABLE_WERROR OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)
FetchContent_Declare(googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.5
        SYSTEM)
FetchContent_MakeAvailable(googlebenchmark)

option(USE_BASELINES "Also benchmark against the oneTBB and libcds hash sets" OFF)
if(USE_BASELINES)
  set(TBB_TEST OFF)
  set(TBB_EXAMPLES OFF)
  set(TBB_STRICT OFF)
  FetchContent_Declare(onetbb
          GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
          GIT_TAG v2022.3.0
          SYSTEM)
  FetchContent_MakeAvailable(onetbb)
  # oneTBB's concurrent_queue.h calls std::equal without including <algorithm>, which libc++ no
  # longer pulls in for it. Only oneTBB's own sources need the include; the headers we use do not.
  target_compile_options(tbb PRIVATE -include algorithm)

  # libcds is fetched but not configured: SOURCE_SUBDIR names a directory with no CMakeLists.txt,
  # so FetchContent populates it and stops. Its own build asks for cmake 3.0.2, forces C++11 and
  # produces a shared library plus tests, none of which is wanted, so the handful of sources Linux
  # needs are built here instead, with the same standard and flags as everything else.
  FetchContent_Declare(libcds
          GIT_REPOSITORY https://github.com/khizmax/libcds.git
          GIT_TAG 9985d2a87feaa3e92532e28f4ab762a82855a49c
          SOURCE_SUBDIR src)
  FetchContent_MakeAvailable(libcds)
  add_library(cds STATIC
          ${libcds_SOURCE_DIR}/src/dhp.cpp
          ${libcds_SOURCE_DIR}/src/hp.cpp
          ${libcds_SOURCE_DIR}/src/hp_thread_local.cpp
          ${libcds_SOURCE_DIR}/src/init.cpp
          ${libcds_SOURCE_DIR}/src/thread_data.cpp
          ${libcds_SOURCE_DIR}/src/topology_linux.cpp
          ${libcds_SOURCE_DIR}/src/urcu_gp.cpp
          ${libcds_SOURCE_DIR}/src/urcu_sh.cpp)
  target_include_directories(cds SYSTEM PUBLIC ${libcds_SOURCE_DIR})
  target_link_libraries(cds PRIVATE Threads::Threads)
  target_compile_options(cds PRIVATE -w)
  target_compile_options(cds PUBLIC -mcx16)
  include(CheckIncludeFileCXX)
  check_include_file_cxx(linux/membarrier.h HAVE_LINUX_MEMBARRIER_H)
  if(HAVE_LINUX_MEMBARRIER_H)
    target_compile_definitions(cds PRIVATE CDS_HAVE_LINUX_MEMBARRIER_H)
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL Clang OR CMAKE_CXX_COMPILER_ID STREQUAL AppleClang)
  add_compile_options(-Werror -Wall -Wextra -pedantic -Weverything)
  add_compile_options(
          -Wno-c++98-compat
          -Wno-c++98-compat-pedantic
          -Wno-padded
          -Wno-unsafe-buffer-usage
          -Wno-thread-safety-negative
  )
  # Clang-23 has new warnings that need suppressing: -Wlifetime-safety-...
  #   -suggestions:  wants to annotate returned references with [[clang::lifetimebound]]
  #   -invalidation:  false positive on reserve() in HashSetRefinable::BuildMutexPointers
  include(CheckCXXCompilerFlag)
  set(CMAKE_REQUIRED_FLAGS -Werror=unknown-warning-option)
  check_cxx_compiler_flag(-Wlifetime-safety-suggestions HAVE_W_LIFETIME_SAFETY_SUGGESTIONS)
  check_cxx_compiler_flag(-Wlifetime-safety-invalidation HAVE_W_LIFETIME_SAFETY_INVALIDATION)
  unset(CMAKE_REQUIRED_FLAGS)
  if(HAVE_W_LIFETIME_SAFETY_SUGGESTIONS)
    add_compile_options(-Wno-lifetime-safety-suggestions)
  endif()
  if(HAVE_W_LIFETIME_SAFETY_INVALIDATION)
    add_compile_options(-Wno-lifetime-safety-invalidation)
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL GNU)
  add_compile_options(-Werror -Wall -Wextra -pedantic)
  add_compile_options(
          -Wcast-align
          -Wcast-qual
          -Wconversion
          -Wdouble-promotion
          -Wduplicated-branches
          -Wduplicated-cond
          -Wextra-semi
          -Wformat=2
          -Wimplicit-fallthrough
          -Wlogical-op
          -Wmisleading-indentation
          -Wnon-virtual-dtor
          -Wnull-dereference
          -Wold-style-cast
          -Woverloaded-virtual
          -Wshadow
          -Wsign-conversion
          -Wsuggest-override
          -Wundef
          -Wunused
          -Wuseless-cast
  )
  # -Winterference-size warns about std::hardware_destructive_interference_size
  add_compile_options(-Wno-interference-size)
else()
  message(FATAL_ERROR "The clang or GCC compiler is required")
endif()

set(USE_SANITIZER "" CACHE STRING
        "Use the compiler sanitizer [asan|tsan]")
if("${USE_SANITIZER}" STREQUAL "asan")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope)
  add_link_options(-fsanitize=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope)
elseif("${USE_SANITIZER}" STREQUAL "tsan")
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
elseif(NOT "${USE_SANITIZER}" STREQUAL "")
  message(FATAL_ERROR "Unknown argument to USE_SANITIZER: ${USE_SANITIZER} - options are [asan|tsan]")
endif()

add_executable(benchmarks
        src/HashSetBase.h
        src/HashSetCoarseGrained.h
        src/HashSetLockFree.h
        src/HashSetRefinable.h
        src/HashSetStriped.h
        src/benchmarks.cc)
target_include_directories(benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(benchmarks PRIVATE Threads::Threads benchmark::benchmark)

if(USE_BASELINES)
  target_sources(benchmarks PRIVATE
          src/HashSetLibcds.h
          src/HashSetTbb.h)
  target_link_libraries(benchmarks PRIVATE TBB::tbb cds)
  target_compile_definitions(benchmarks PRIVATE USE_BASELINES)
endif()
