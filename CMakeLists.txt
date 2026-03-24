cmake_minimum_required(VERSION 3.22)
project(nitro)

set(CMAKE_CXX_STANDARD 26)
add_compile_options(-Wno-interference-size -march=native -flto)
add_link_options(-flto)

# pump framework
include_directories(pump/src)

# Dependencies from pump
find_package(PkgConfig REQUIRED)
include(pump/src/env/scheduler/nvme/nvme.cmake)
include(pump/src/env/scheduler/net/tcp/tcp.cmake)
include(pump/src/env/scheduler/net/udp/udp.cmake)
include(pump/src/env/scheduler/cuda/cuda.cmake)

# KV
add_executable(kv apps/kv/main.cc)
target_include_directories(kv PRIVATE ${CMAKE_SOURCE_DIR}/apps/kv)
target_link_libraries(kv PRIVATE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        pthread isal ssl rt uuid crypto m aio numa dl)

# AiSAQ
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/aisaq_kernels.ptx
    COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} --ptx
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/aisaq/gpu/kernels.cu
        -o ${CMAKE_CURRENT_BINARY_DIR}/aisaq_kernels.ptx
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/apps/aisaq/gpu/kernels.cu
)
add_custom_target(aisaq_kernels_ptx DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/aisaq_kernels.ptx)

add_executable(aisaq apps/aisaq/main.cc)
add_dependencies(aisaq aisaq_kernels_ptx)
target_include_directories(aisaq PRIVATE ${CMAKE_SOURCE_DIR}/apps/aisaq ${CMAKE_SOURCE_DIR})
target_link_libraries(aisaq PRIVATE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        CUDA::cuda_driver
        pthread isal ssl rt uuid crypto m aio numa dl)

# Sider
add_executable(sider apps/sider/main.cc)
target_include_directories(sider PRIVATE ${CMAKE_SOURCE_DIR}/apps/sider ${CMAKE_SOURCE_DIR})
target_link_libraries(sider PRIVATE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        ${URING_LIBRARY} pthread numa dl)

# Sider tests
add_executable(sider_test_store apps/sider/test_store.cc)
target_include_directories(sider_test_store PRIVATE ${CMAKE_SOURCE_DIR}/apps/sider)
target_link_libraries(sider_test_store PRIVATE pthread)

# Sider benchmark (standalone, no SPDK — based on Valkey redis-benchmark, BSD-3-Clause)
set(SIDER_BENCH_DIR ${CMAKE_SOURCE_DIR}/apps/sider/bench)
add_executable(sider_bench
    ${SIDER_BENCH_DIR}/sider-benchmark.c
    ${SIDER_BENCH_DIR}/ae.c
    ${SIDER_BENCH_DIR}/anet.c
    ${SIDER_BENCH_DIR}/adlist.c
    ${SIDER_BENCH_DIR}/dict.c
    ${SIDER_BENCH_DIR}/zmalloc.c
    ${SIDER_BENCH_DIR}/serverassert.c
    ${SIDER_BENCH_DIR}/release.c
    ${SIDER_BENCH_DIR}/crcspeed.c
    ${SIDER_BENCH_DIR}/crccombine.c
    ${SIDER_BENCH_DIR}/crc64.c
    ${SIDER_BENCH_DIR}/siphash.c
    ${SIDER_BENCH_DIR}/crc16.c
    ${SIDER_BENCH_DIR}/monotonic.c
    ${SIDER_BENCH_DIR}/cli_common.c
    ${SIDER_BENCH_DIR}/mt19937-64.c
    ${SIDER_BENCH_DIR}/strl.c
    ${SIDER_BENCH_DIR}/hdr_histogram.c)
target_include_directories(sider_bench PRIVATE ${SIDER_BENCH_DIR} /usr/include/hiredis)
target_link_libraries(sider_bench PRIVATE hiredis pthread m)
set_target_properties(sider_bench PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON)
target_compile_options(sider_bench PRIVATE -Wno-unused-result -Wno-deprecated-declarations)

# AiSAQ tools
add_executable(aisaq_gen_test apps/aisaq/tools/gen_test_index.cc)
target_link_libraries(aisaq_gen_test PRIVATE pthread)
add_executable(aisaq_convert_index apps/aisaq/tools/convert_index.cc)
target_link_libraries(aisaq_convert_index PRIVATE pthread)
