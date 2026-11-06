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
  src/core/kv.c
  src/core/session.c
  src/core/sha256.c
  src/storage/schema.c
  src/memory/memory.c
  src/cache/cache.c
  src/ingest/import.c
  src/ingest/structured.c
  src/vector/kernels.c
  src/vector/search.c
  src/core/pool.c
  src/retrieval/lexical.c
  src/vector/store.c
  src/retrieval/hybrid.c
  src/semantic/linalg.c
  src/semantic/sparse.c
  src/semantic/rsvd.c
  src/semantic/semantic.c
  src/semantic/port.c
  src/vector/backend.c)
target_include_directories(xksm
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
if(TARGET SQLite3::SQLite3)  # CMake >= 4.3 name; older CMake only defines SQLite::SQLite3
  target_link_libraries(xksm PUBLIC SQLite3::SQLite3 Threads::Threads m)
else()
  target_link_libraries(xksm PUBLIC SQLite::SQLite3 Threads::Threads m)
endif()
target_compile_options(xksm PRIVATE ${XKSM_WARN})

add_executable(xksm_cli cli/main.c cli/harness.c)
set_target_properties(xksm_cli PROPERTIES OUTPUT_NAME xksm)
target_link_libraries(xksm_cli PRIVATE xksm)
target_compile_options(xksm_cli PRIVATE ${XKSM_WARN})

enable_testing()
foreach(t schema sha256 memory cache import structured agents vector search lexical vecstore hybrid linalg semantic)
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
add_test(NAME structured COMMAND test_structured ${CMAKE_CURRENT_BINARY_DIR}/test_structured.xksm)
add_test(NAME agents COMMAND test_agents ${CMAKE_CURRENT_BINARY_DIR}/test_agents.xksm)
add_test(NAME cli_harness COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_harness.sh $<TARGET_FILE:xksm_cli> ${CMAKE_CURRENT_BINARY_DIR}/cli_harness)
add_test(NAME vector COMMAND test_vector)
add_test(NAME search COMMAND test_search ${CMAKE_CURRENT_BINARY_DIR}/test_search.xksm)
add_test(NAME lexical COMMAND test_lexical ${CMAKE_CURRENT_BINARY_DIR}/test_lexical.xksm)
add_test(NAME vecstore COMMAND test_vecstore ${CMAKE_CURRENT_BINARY_DIR}/test_vecstore.xksm)
add_test(NAME hybrid COMMAND test_hybrid ${CMAKE_CURRENT_BINARY_DIR}/test_hybrid.xksm)
add_test(NAME linalg COMMAND test_linalg)
add_test(NAME semantic COMMAND test_semantic ${CMAKE_CURRENT_BINARY_DIR}/test_semantic.xksm)

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

add_executable(bench_wiki benchmarks/bench_wiki.c)
target_link_libraries(bench_wiki PRIVATE xksm)
target_compile_options(bench_wiki PRIVATE ${XKSM_WARN})

add_executable(eval_retrieval benchmarks/eval_retrieval.c)
target_link_libraries(eval_retrieval PRIVATE xksm)
target_compile_options(eval_retrieval PRIVATE ${XKSM_WARN})

# Optional OpenCL GPU backend (randomized SVD of the semantic model). Built when OpenCL
# headers and the ICD loader are found; the CPU path stays complete either way.
option(XKSM_OPENCL "Build the OpenCL GPU backend when OpenCL is available" ON)
if(XKSM_OPENCL)
  find_package(OpenCL QUIET)
  if(OpenCL_FOUND)
    set(CL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/backends/opencl/rsvd.cl)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CL_FILE})
    file(READ ${CL_FILE} CL_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CL_ARR "${CL_HEX}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/rsvd_cl.h
         "/* generated from backends/opencl/rsvd.cl */\nstatic const unsigned char k_rsvd_cl[] = {${CL_ARR} 0x00};\n")
    target_sources(xksm PRIVATE backends/opencl/ocl_rsvd.c)
    target_include_directories(xksm PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(xksm PRIVATE XKSM_HAVE_OPENCL=1)
    target_link_libraries(xksm PUBLIC OpenCL::OpenCL)
    message(STATUS "xksm: OpenCL backend enabled (${OpenCL_VERSION_STRING})")
  else()
    message(STATUS "xksm: OpenCL not found, GPU backend disabled")
  endif()
endif()
if(XKSM_OPENCL AND OpenCL_FOUND)
  add_executable(test_ocl tests/test_ocl.c)
  target_link_libraries(test_ocl PRIVATE xksm)
  target_include_directories(test_ocl PRIVATE src/core)
  target_compile_options(test_ocl PRIVATE ${XKSM_WARN})
  add_test(NAME ocl COMMAND test_ocl)
endif()

add_executable(bench_semantic benchmarks/bench_semantic.c)
target_link_libraries(bench_semantic PRIVATE xksm)
target_compile_options(bench_semantic PRIVATE ${XKSM_WARN})
# tests that may initialize an OpenCL platform: driver-internal allocations are suppressed
set_property(TEST semantic APPEND PROPERTY ENVIRONMENT
             "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
if(TARGET test_ocl)
  set_property(TEST ocl APPEND PROPERTY ENVIRONMENT
               "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
endif()

# Optional comparison harness against a llama.cpp embedding model:
#   -DXKSM_LLAMA_DIR=<llama.cpp source> -DXKSM_LLAMA_BUILD=<its build dir>
set(XKSM_LLAMA_DIR "" CACHE PATH "llama.cpp source tree for the embed_llama benchmark")
set(XKSM_LLAMA_BUILD "" CACHE PATH "llama.cpp build tree (bin/libllama.so)")
if(XKSM_LLAMA_DIR AND XKSM_LLAMA_BUILD)
  add_executable(embed_llama benchmarks/embed_llama.c)
  target_include_directories(embed_llama SYSTEM PRIVATE ${XKSM_LLAMA_DIR}/include
                             ${XKSM_LLAMA_DIR}/ggml/include)
  target_link_libraries(embed_llama PRIVATE xksm ${XKSM_LLAMA_BUILD}/bin/libllama.so)
  set_target_properties(embed_llama PROPERTIES BUILD_RPATH ${XKSM_LLAMA_BUILD}/bin)
  target_compile_options(embed_llama PRIVATE ${XKSM_WARN})
endif()
add_executable(static_gguf benchmarks/static_gguf.c)
target_link_libraries(static_gguf PRIVATE xksm m)
target_compile_options(static_gguf PRIVATE ${XKSM_WARN})
if(XKSM_LLAMA_DIR AND XKSM_LLAMA_BUILD)
  # same tool, plus a token-by-token comparison with llama.cpp's tokenizer
  add_executable(distill_llama benchmarks/distill_llama.c)
  target_include_directories(distill_llama SYSTEM PRIVATE ${XKSM_LLAMA_DIR}/include
                             ${XKSM_LLAMA_DIR}/ggml/include)
  target_link_libraries(distill_llama PRIVATE ${XKSM_LLAMA_BUILD}/bin/libllama.so)
  set_target_properties(distill_llama PROPERTIES BUILD_RPATH ${XKSM_LLAMA_BUILD}/bin)
  target_compile_options(distill_llama PRIVATE ${XKSM_WARN})
  add_executable(relate_llama benchmarks/relate_llama.c)
  target_include_directories(relate_llama SYSTEM PRIVATE ${XKSM_LLAMA_DIR}/include
                             ${XKSM_LLAMA_DIR}/ggml/include)
  target_link_libraries(relate_llama PRIVATE m sqlite3 ${XKSM_LLAMA_BUILD}/bin/libllama.so)
  set_target_properties(relate_llama PROPERTIES BUILD_RPATH ${XKSM_LLAMA_BUILD}/bin)
  target_compile_options(relate_llama PRIVATE ${XKSM_WARN})
  add_executable(port_build tools/port_build.c)
  target_include_directories(port_build SYSTEM PRIVATE ${XKSM_LLAMA_DIR}/include
                             ${XKSM_LLAMA_DIR}/ggml/include)
  target_link_libraries(port_build PRIVATE m sqlite3 ${XKSM_LLAMA_BUILD}/bin/libllama.so)
  set_target_properties(port_build PROPERTIES BUILD_RPATH ${XKSM_LLAMA_BUILD}/bin)
  target_compile_options(port_build PRIVATE ${XKSM_WARN})
  add_executable(static_gguf_check benchmarks/static_gguf.c)
  target_compile_definitions(static_gguf_check PRIVATE XKSM_WITH_LLAMA=1)
  target_include_directories(static_gguf_check SYSTEM PRIVATE ${XKSM_LLAMA_DIR}/include
                             ${XKSM_LLAMA_DIR}/ggml/include)
  target_link_libraries(static_gguf_check PRIVATE xksm m ${XKSM_LLAMA_BUILD}/bin/libllama.so)
  set_target_properties(static_gguf_check PROPERTIES BUILD_RPATH ${XKSM_LLAMA_BUILD}/bin)
  target_compile_options(static_gguf_check PRIVATE ${XKSM_WARN})
endif()

# ---- install: the application (binary, agent integrations, docs) and the library ----------
# Application files go to share/xks-memory; memories are data, never installed: the CLI keeps
# them in $XDG_DATA_HOME/xksm (default
# ~/.local/share/xksm/agents.xksm).
include(GNUInstallDirs)
install(TARGETS xksm_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS xksm ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/xksm.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY integrations/claude-code/ DESTINATION ${CMAKE_INSTALL_DATADIR}/xks-memory/claude-code
        USE_SOURCE_PERMISSIONS)
install(DIRECTORY integrations/omp/ DESTINATION ${CMAKE_INSTALL_DATADIR}/xks-memory/omp
        USE_SOURCE_PERMISSIONS OPTIONAL)
install(FILES README.md LICENSE docs/EMBEDDING.md docs/AGENTS_INTEGRATION.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
