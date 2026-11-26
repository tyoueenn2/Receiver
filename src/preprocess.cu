#include <cstdint>
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include "engine_precision_kernel.cuh"
#include "preprocess_kernel.cuh"
namespace receiver {
void launch_half_to_float(const void* source, float* target, size_t count, cudaStream_t stream) {
    half_to_float<<<unsigned((count + 255) / 256), 256, 0, stream>>>(static_cast<const __half*>(source),
                                                                     target, count);
}
void launch_preprocess(const uint8_t* raw, void* tensor, bool half, int w, int h, int channels, int size,
                       int rw, int rh, int left, int top, cudaStream_t stream) {
    const dim3 grid((size + 15) / 16, (size + 15) / 16), block(16, 16);
    if (half)
        preprocess_half_kernel<<<grid, block, 0, stream>>>(
            raw, static_cast<__half*>(tensor), w, h, channels, size, rw, rh, left, top);
    else
        preprocess_kernel<<<grid, block, 0, stream>>>(
            raw, static_cast<float*>(tensor), w, h, channels, size, rw, rh, left, top);
}
} // namespace receiver
