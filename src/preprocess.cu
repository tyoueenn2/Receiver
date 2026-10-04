#include <cstdint>
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include "engine_precision_kernel.cuh"
#include "preprocess_kernel.cuh"
namespace receiver {
void launch_float_to_half(const float* source, void* target, size_t count, cudaStream_t stream) {
    float_to_half<<<unsigned((count + 255) / 256), 256, 0, stream>>>(source, static_cast<__half*>(target),
                                                                     count);
}
void launch_half_to_float(const void* source, float* target, size_t count, cudaStream_t stream) {
    half_to_float<<<unsigned((count + 255) / 256), 256, 0, stream>>>(static_cast<const __half*>(source),
                                                                     target, count);
}
void launch_preprocess(const uint8_t* raw, float* tensor, int w, int h, int channels, int size, int rw,
                       int rh, int left, int top, cudaStream_t stream) {
    preprocess_kernel<<<dim3((size + 15) / 16, (size + 15) / 16), dim3(16, 16), 0, stream>>>(
        raw, tensor, w, h, channels, size, rw, rh, left, top);
}
} // namespace receiver
