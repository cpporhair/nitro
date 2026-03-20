cmake_minimum_required(VERSION 3.22)
project(nitro)

set(CMAKE_CXX_STANDARD 26)
add_compile_options(-Wno-interference-size -march=native)

# pump framework
include_directories(pump/src)

# Dependencies from pump
find_package(PkgConfig REQUIRED)
include(pump/src/env/scheduler/nvme/nvme.cmake)
include(pump/src/env/scheduler/net/tcp/tcp.cmake)
include(pump/src/env/scheduler/net/udp/udp.cmake)
include(pump/src/env/scheduler/cuda/cuda.cmake)

# KV
add_executable(kv kv/main.cc)
target_include_directories(kv PRIVATE ${CMAKE_SOURCE_DIR}/kv)
target_link_libraries(kv PRIVATE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        pthread isal ssl rt uuid crypto m aio numa dl)

# AiSAQ
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/aisaq_kernels.ptx
    COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} --ptx
        ${CMAKE_CURRENT_SOURCE_DIR}/aisaq/gpu/kernels.cu
        -o ${CMAKE_CURRENT_BINARY_DIR}/aisaq_kernels.ptx
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/aisaq/gpu/kernels.cu
)
add_custom_target(aisaq_kernels_ptx DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/aisaq_kernels.ptx)

add_executable(aisaq aisaq/main.cc)
add_dependencies(aisaq aisaq_kernels_ptx)
target_include_directories(aisaq PRIVATE ${CMAKE_SOURCE_DIR}/aisaq ${CMAKE_SOURCE_DIR})
target_link_libraries(aisaq PRIVATE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        CUDA::cuda_driver
        pthread isal ssl rt uuid crypto m aio numa dl)

# AiSAQ tools
add_executable(aisaq_gen_test aisaq/tools/gen_test_index.cc)
target_link_libraries(aisaq_gen_test PRIVATE pthread)
add_executable(aisaq_convert_index aisaq/tools/convert_index.cc)
target_link_libraries(aisaq_convert_index PRIVATE pthread)
