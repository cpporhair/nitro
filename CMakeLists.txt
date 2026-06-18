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

# Abseil — used by apps/inconel/* for flat_hash_map / flat_hash_set /
# InlinedVector. Step 008 only pulls the container subset; CRC migration
# (absl::ComputeCrc32c) is intentionally left to INC-032.
find_package(absl CONFIG REQUIRED)

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

# Inconel real-NVMe test
set(INCONEL_ABSL_LIBS
    absl::flat_hash_map
    absl::flat_hash_set
    absl::inlined_vector
    absl::crc32c)

add_library(inconel_real_nvme INTERFACE)
target_link_libraries(inconel_real_nvme INTERFACE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        pthread isal ssl rt uuid crypto m aio numa dl)

add_library(inconel_real_nvme_compile_check OBJECT
        apps/inconel/nvme/real_compile_check.cc)
target_include_directories(inconel_real_nvme_compile_check PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_real_nvme_compile_check PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_flush_e2e apps/inconel/test/test_flush_e2e.cc)
target_include_directories(inconel_test_flush_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_flush_e2e PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

# 058 steady-state background-loop e2e on the real SPDK NVMe backend.
# Needs --pci-addr BDF (or INCONEL_NVME_PCI_ADDR); force-formats the
# target namespace, so point it at a scratch device only.
add_executable(inconel_test_steady_e2e apps/inconel/test/test_steady_e2e.cc)
target_include_directories(inconel_test_steady_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_steady_e2e PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_concurrent_runtime_e2e
        apps/inconel/test/test_concurrent_runtime_e2e.cc)
target_include_directories(inconel_test_concurrent_runtime_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_concurrent_runtime_e2e PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_value_placement_e2e
        apps/inconel/test/test_value_placement_e2e.cc)
target_include_directories(inconel_test_value_placement_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_value_placement_e2e PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_multishard_split_e2e
        apps/inconel/test/test_multishard_split_e2e.cc)
target_include_directories(inconel_test_multishard_split_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_multishard_split_e2e PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_seal_inflight_race_e2e
        apps/inconel/test/test_seal_inflight_race_e2e.cc)
target_include_directories(inconel_test_seal_inflight_race_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_seal_inflight_race_e2e PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_write_backpressure_e2e
        apps/inconel/test/test_write_backpressure_e2e.cc)
target_include_directories(inconel_test_write_backpressure_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_write_backpressure_e2e PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

# INC-050 direction B: CRC32C bit-identical-to-absl compatibility gate (053 §7).
add_executable(inconel_test_crc32c apps/inconel/test/test_crc32c.cc)
target_include_directories(inconel_test_crc32c PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_crc32c PRIVATE ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m01_batch_carrier_memtable
        apps/inconel/test/test_m01_batch_carrier_memtable.cc)
target_include_directories(inconel_test_m01_batch_carrier_memtable PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m01_batch_carrier_memtable PRIVATE
        ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m02_read_handle_prs_memtable_lookup
        apps/inconel/test/test_m02_read_handle_prs_memtable_lookup.cc)
target_include_directories(inconel_test_m02_read_handle_prs_memtable_lookup PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m02_read_handle_prs_memtable_lookup PRIVATE
        ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m03_coord_scheduler_assign_publish_release
        apps/inconel/test/test_m03_coord_scheduler_assign_publish_release.cc)
target_include_directories(inconel_test_m03_coord_scheduler_assign_publish_release PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m03_coord_scheduler_assign_publish_release PRIVATE
        ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m04_wal_space_scheduler
        apps/inconel/test/test_m04_wal_space_scheduler.cc)
target_include_directories(inconel_test_m04_wal_space_scheduler PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m04_wal_space_scheduler PRIVATE
        ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m05_front_scheduler_memtable_owner
        apps/inconel/test/test_m05_front_scheduler_memtable_owner.cc)
target_include_directories(inconel_test_m05_front_scheduler_memtable_owner PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m05_front_scheduler_memtable_owner PRIVATE
        ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m06_front_wal_append_prepare
        apps/inconel/test/test_m06_front_wal_append_prepare.cc)
target_include_directories(inconel_test_m06_front_wal_append_prepare PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m06_front_wal_append_prepare PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m07_value_persist_read_adapter
        apps/inconel/test/test_m07_value_persist_read_adapter.cc)
target_include_directories(inconel_test_m07_value_persist_read_adapter PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m07_value_persist_read_adapter PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m08_write_baseline_inflight
        apps/inconel/test/test_m08_write_baseline_inflight.cc)
target_include_directories(inconel_test_m08_write_baseline_inflight PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m08_write_baseline_inflight PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m09_production_write_batch
        apps/inconel/test/test_m09_production_write_batch.cc)
target_include_directories(inconel_test_m09_production_write_batch PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m09_production_write_batch PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m10_point_get_live_read
        apps/inconel/test/test_m10_point_get_live_read.cc)
target_include_directories(inconel_test_m10_point_get_live_read PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m10_point_get_live_read PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m11_runtime_topology_operations
        apps/inconel/test/test_m11_runtime_topology_operations.cc)
target_include_directories(inconel_test_m11_runtime_topology_operations PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m11_runtime_topology_operations PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m12_seal_round
        apps/inconel/test/test_m12_seal_round.cc)
target_include_directories(inconel_test_m12_seal_round PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m12_seal_round PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

add_executable(inconel_test_m14_wal_group_commit
        apps/inconel/test/test_m14_wal_group_commit.cc)
target_include_directories(inconel_test_m14_wal_group_commit PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(inconel_test_m14_wal_group_commit PRIVATE
        inconel_real_nvme ${INCONEL_ABSL_LIBS})

# AiSAQ tools
add_executable(aisaq_gen_test apps/aisaq/tools/gen_test_index.cc)
target_link_libraries(aisaq_gen_test PRIVATE pthread)
add_executable(aisaq_convert_index apps/aisaq/tools/convert_index.cc)
target_link_libraries(aisaq_convert_index PRIVATE pthread)
