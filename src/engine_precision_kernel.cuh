#pragma once
#include <cuda_fp16.h>
namespace receiver {
extern "C" __global__ void float_to_half(const float* source, __half* target, size_t count) {
    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < count)
        target[i] = __float2half_rn(source[i]);
}
extern "C" __global__ void half_to_float(const __half* source, float* target, size_t count) {
    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < count)
        target[i] = __half2float(source[i]);
}
} // namespace receiver
