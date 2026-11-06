cmake_minimum_required(VERSION 3.20)
project(xks_memory VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(XKSM_CUDA "Build the optional CUDA backend" OFF)
option(XKSM_KNC "Build the optional Knights Corner backend (experimental)" OFF)
option(XKSM_WERROR "Treat warnings as errors" ON)

find_package(SQLite3 3.35 REQUIRED)
find_package(Threads REQUIRED)

set(XKSM_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes)
if(XKSM_WERROR)
  list(APPEND XKSM_WARN -Werror)
endif()

add_library(xksm
  src/core/xksm.c
  src/core/sha256.c
  src/storage/schema.c
  src/memory/memory.c
  src/cache/cache.c
  src/ingest/import.c
  src/vector/kernels.c
  src/vector/search.c
  src/core/pool.c
  src/retrieval/lexical.c
  src/vector/store.c)
target_include_directories(xksm
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
if(TARGET SQLite3::SQLite3)  # CMake >= 4.3 name; older CMake only defines SQLite::SQLite3
  target_link_libraries(xksm PUBLIC SQLite3::SQLite3 Threads::Threads m)
else()
  target_link_libraries(xksm PUBLIC SQLite::SQLite3 Threads::Threads m)
endif()
target_compile_options(xksm PRIVATE ${XKSM_WARN})

add_executable(xksm_cli cli/main.c)
set_target_properties(xksm_cli PROPERTIES OUTPUT_NAME xksm)
target_link_libraries(xksm_cli PRIVATE xksm)
target_compile_options(xksm_cli PRIVATE ${XKSM_WARN})

enable_testing()
foreach(t schema sha256 memory cache import vector search lexical vecstore)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE xksm)
  target_include_directories(test_${t} PRIVATE src/core)
  target_compile_options(test_${t} PRIVATE ${XKSM_WARN})
endforeach()
add_test(NAME schema COMMAND test_schema ${CMAKE_CURRENT_BINARY_DIR}/test_schema.xksm)
add_test(NAME sha256 COMMAND test_sha256)
add_test(NAME memory COMMAND test_memory ${CMAKE_CURRENT_BINARY_DIR}/test_memory.xksm)
add_test(NAME cache COMMAND test_cache ${CMAKE_CURRENT_BINARY_DIR}/test_cache.xksm)
add_test(NAME import COMMAND test_import ${CMAKE_CURRENT_BINARY_DIR}/test_import.xksm)
add_test(NAME vector COMMAND test_vector)
add_test(NAME search COMMAND test_search ${CMAKE_CURRENT_BINARY_DIR}/test_search.xksm)
add_test(NAME lexical COMMAND test_lexical ${CMAKE_CURRENT_BINARY_DIR}/test_lexical.xksm)
add_test(NAME vecstore COMMAND test_vecstore ${CMAKE_CURRENT_BINARY_DIR}/test_vecstore.xksm)

add_executable(bench_sha256 benchmarks/bench_sha256.c)
target_include_directories(bench_sha256 PRIVATE src/core)
target_link_libraries(bench_sha256 PRIVATE xksm)
target_compile_options(bench_sha256 PRIVATE ${XKSM_WARN})

add_executable(bench_insert benchmarks/bench_insert.c)
target_link_libraries(bench_insert PRIVATE xksm)
target_compile_options(bench_insert PRIVATE ${XKSM_WARN})

add_executable(bench_get benchmarks/bench_get.c)
target_link_libraries(bench_get PRIVATE xksm)
target_compile_options(bench_get PRIVATE ${XKSM_WARN})

add_executable(bench_vector benchmarks/bench_vector.c)
target_link_libraries(bench_vector PRIVATE xksm)
target_compile_options(bench_vector PRIVATE ${XKSM_WARN})

add_executable(bench_threads benchmarks/bench_threads.c)
target_link_libraries(bench_threads PRIVATE xksm)
target_compile_options(bench_threads PRIVATE ${XKSM_WARN})

add_executable(bench_search benchmarks/bench_search.c)
target_link_libraries(bench_search PRIVATE xksm)
target_compile_options(bench_search PRIVATE ${XKSM_WARN})
