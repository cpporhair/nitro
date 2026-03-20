#ifndef AISAQ_GPU_KERNELS_HH
#define AISAQ_GPU_KERNELS_HH

#include <cuda.h>
#include <stdexcept>

namespace aisaq::gpu {

    inline void check_cu(CUresult r, const char* msg = "CUDA error") {
        if (r != CUDA_SUCCESS) {
            const char* err = nullptr;
            cuGetErrorString(r, &err);
            throw std::runtime_error(std::string(msg) + ": " + (err ? err : "unknown"));
        }
    }

    struct kernels {
        CUmodule module = nullptr;
        CUfunction mean_reduce = nullptr;
        CUfunction kmeans_pp_dist = nullptr;
        CUfunction kmeans_assign_accum = nullptr;
        CUfunction kmeans_divide = nullptr;
        CUfunction pq_encode = nullptr;

        void load(const char* ptx_path) {
            check_cu(cuModuleLoad(&module, ptx_path), "cuModuleLoad");
            check_cu(cuModuleGetFunction(&mean_reduce, module, "mean_reduce"));
            check_cu(cuModuleGetFunction(&kmeans_pp_dist, module, "kmeans_pp_dist"));
            check_cu(cuModuleGetFunction(&kmeans_assign_accum, module, "kmeans_assign_accum"));
            check_cu(cuModuleGetFunction(&kmeans_divide, module, "kmeans_divide"));
            check_cu(cuModuleGetFunction(&pq_encode, module, "pq_encode"));
        }

        ~kernels() {
            if (module) cuModuleUnload(module);
        }

        kernels() = default;
        kernels(const kernels&) = delete;
        kernels& operator=(const kernels&) = delete;
    };

    // RAII GPU context for synchronous PQ training
    struct gpu_context {
        CUdevice device;
        CUcontext ctx;
        CUstream stream;
        kernels kern;

        gpu_context(int device_id, const char* ptx_path) {
            check_cu(cuInit(0), "cuInit");
            check_cu(cuDeviceGet(&device, device_id), "cuDeviceGet");
            check_cu(cuCtxCreate(&ctx, nullptr, 0, device), "cuCtxCreate");
            check_cu(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
            kern.load(ptx_path);

            char name[256];
            cuDeviceGetName(name, sizeof(name), device);
            printf("  GPU %d: %s\n", device_id, name);
        }

        ~gpu_context() {
            cuStreamDestroy(stream);
            cuCtxDestroy(ctx);
        }

        gpu_context(const gpu_context&) = delete;
        gpu_context& operator=(const gpu_context&) = delete;

        void sync() { check_cu(cuStreamSynchronize(stream), "cuStreamSync"); }

        // Convenience launch wrapper
        template<typename... Args>
        void launch(CUfunction f, unsigned grid, unsigned block, Args... args) {
            void* params[] = { (void*)&args... };
            check_cu(cuLaunchKernel(f, grid, 1, 1, block, 1, 1,
                                    0, stream, params, nullptr), "cuLaunchKernel");
        }

        CUdeviceptr alloc(size_t bytes) {
            CUdeviceptr ptr;
            check_cu(cuMemAlloc(&ptr, bytes), "cuMemAlloc");
            return ptr;
        }

        void free(CUdeviceptr ptr) { cuMemFree(ptr); }

        void upload(CUdeviceptr dst, const void* src, size_t bytes) {
            check_cu(cuMemcpyHtoDAsync(dst, src, bytes, stream), "H2D");
        }

        void download(void* dst, CUdeviceptr src, size_t bytes) {
            check_cu(cuMemcpyDtoHAsync(dst, src, bytes, stream), "D2H");
        }

        void zero(CUdeviceptr ptr, size_t bytes) {
            check_cu(cuMemsetD8Async(ptr, 0, bytes, stream), "memset");
        }
    };

}

#endif // AISAQ_GPU_KERNELS_HH
