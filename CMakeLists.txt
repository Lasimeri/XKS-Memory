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

set(XKSM_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes)
if(XKSM_WERROR)
  list(APPEND XKSM_WARN -Werror)
endif()

add_library(xksm
  src/core/xksm.c
  src/core/sha512.c
  src/storage/schema.c)
target_include_directories(xksm
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
if(TARGET SQLite3::SQLite3)  # CMake >= 4.3 name; older CMake only defines SQLite::SQLite3
  target_link_libraries(xksm PUBLIC SQLite3::SQLite3)
else()
  target_link_libraries(xksm PUBLIC SQLite::SQLite3)
endif()
target_compile_options(xksm PRIVATE ${XKSM_WARN})

add_executable(xksm_cli cli/main.c)
set_target_properties(xksm_cli PROPERTIES OUTPUT_NAME xksm)
target_link_libraries(xksm_cli PRIVATE xksm)
target_compile_options(xksm_cli PRIVATE ${XKSM_WARN})

enable_testing()
foreach(t schema sha512)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE xksm)
  target_include_directories(test_${t} PRIVATE src/core)
  target_compile_options(test_${t} PRIVATE ${XKSM_WARN})
endforeach()
add_test(NAME schema COMMAND test_schema ${CMAKE_CURRENT_BINARY_DIR}/test_schema.xksm)
add_test(NAME sha512 COMMAND test_sha512)

add_executable(bench_sha512 benchmarks/bench_sha512.c)
target_include_directories(bench_sha512 PRIVATE src/core)
target_link_libraries(bench_sha512 PRIVATE xksm)
target_compile_options(bench_sha512 PRIVATE ${XKSM_WARN})

if(XKSM_CUDA)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 86)  # RTX 30-series; override with -DCMAKE_CUDA_ARCHITECTURES
  endif()
  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 17)
  add_executable(bench_sha512_cuda benchmarks/bench_sha512_cuda.cu)
  target_include_directories(bench_sha512_cuda PRIVATE src/core)
  target_link_libraries(bench_sha512_cuda PRIVATE xksm)
  target_compile_options(bench_sha512_cuda PRIVATE -O3 -Xcompiler=-Wall,-Wextra)
endif()
