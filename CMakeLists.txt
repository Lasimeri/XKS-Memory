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
  src/storage/schema.c)
target_include_directories(xksm
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(xksm PUBLIC SQLite::SQLite3)
target_compile_options(xksm PRIVATE ${XKSM_WARN})

add_executable(xksm_cli cli/main.c)
set_target_properties(xksm_cli PROPERTIES OUTPUT_NAME xksm)
target_link_libraries(xksm_cli PRIVATE xksm)
target_compile_options(xksm_cli PRIVATE ${XKSM_WARN})

enable_testing()
foreach(t schema)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE xksm)
  target_compile_options(test_${t} PRIVATE ${XKSM_WARN})
  add_test(NAME ${t} COMMAND test_${t} ${CMAKE_CURRENT_BINARY_DIR}/test_${t}.xksm)
endforeach()
