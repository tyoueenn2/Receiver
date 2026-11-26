#pragma once
#include <cuda_fp16.h>

namespace receiver {
__device__ __forceinline__ void store_normalized(float* tensor, int index, float value) {
    tensor[index] = value / 255.f;
}
__device__ __forceinline__ void store_normalized(__half* tensor, int index, float value) {
    tensor[index] = __float2half_rn(value / 255.f);
}

template <typename Pixel>
__device__ __forceinline__ void preprocess_pixel(const unsigned char* raw, Pixel* tensor, int w, int h,
                                                 int channels, int size, int rw, int rh, int left, int top) {
    const int x = int(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = int(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= size || y >= size)
        return;
    const int index = y * size + x;
    const int plane = size * size;
    const int col = x - left, row = y - top;
    if (col < 0 || col >= rw || row < 0 || row >= rh) {
        for (int c = 0; c < 3; ++c)
            store_normalized(tensor, c * plane + index, 114.f);
        return;
    }
    if (w == rw && h == rh) {
        // An unscaled crop lands at integer source coordinates.
        const int source = row * w + col;
        if (channels == 4) {
            // The upload buffer and every BGRA pixel are four-byte aligned.
            const auto pixel = reinterpret_cast<const uchar4*>(raw)[source];
            store_normalized(tensor, index, pixel.z);
            store_normalized(tensor, plane + index, pixel.y);
            store_normalized(tensor, 2 * plane + index, pixel.x);
        } else {
            const auto* pixel = raw + 3 * source;
            for (int c = 0; c < 3; ++c)
                store_normalized(tensor, c * plane + index, pixel[c]);
        }
        return;
    }
    const float sx = fminf(fmaxf((col + .5f) * w / rw - .5f, 0.f), float(w - 1));
    const float sy = fminf(fmaxf((row + .5f) * h / rh - .5f, 0.f), float(h - 1));
    const int x0 = int(sx), y0 = int(sy);
    const int x1 = x0 + 1 < w ? x0 + 1 : w - 1, y1 = y0 + 1 < h ? y0 + 1 : h - 1;
    const float fx = sx - x0, fy = sy - y0;
    for (int c = 0; c < 3; ++c) {
        const int channel = channels == 4 ? 2 - c : c;
        const float a = raw[(y0 * w + x0) * channels + channel];
        const float b = raw[(y0 * w + x1) * channels + channel];
        const float d = raw[(y1 * w + x0) * channels + channel];
        const float e = raw[(y1 * w + x1) * channels + channel];
        const float value = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
        store_normalized(tensor, c * plane + index, value);
    }
}

extern "C" __global__ void preprocess_kernel(const unsigned char* raw, float* tensor, int w, int h,
                                             int channels, int size, int rw, int rh, int left, int top) {
    preprocess_pixel(raw, tensor, w, h, channels, size, rw, rh, left, top);
}

extern "C" __global__ void preprocess_half_kernel(const unsigned char* raw, __half* tensor, int w, int h,
                                                  int channels, int size, int rw, int rh, int left, int top) {
    preprocess_pixel(raw, tensor, w, h, channels, size, rw, rh, left, top);
}
} // namespace receiver
