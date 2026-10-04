"""Execute production float/half conversion kernels through the Windows CUDA driver."""
import argparse
import ctypes as c
import math
from pathlib import Path
import random
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ptx')
    args = parser.parse_args()
    cuda = c.WinDLL('nvcuda.dll')

    def call(name, types, *values):
        function = getattr(cuda, name)
        function.argtypes, function.restype = types, c.c_int
        result = function(*values)
        if result: raise RuntimeError(f'{name}: CUDA status {result}')

    context, module = c.c_void_p(), c.c_void_p()
    device = c.c_int()
    buffers = []
    call('cuInit', [c.c_uint], 0)
    call('cuDeviceGet', [c.POINTER(c.c_int), c.c_int], c.byref(device), 0)
    call('cuCtxCreate_v2', [c.POINTER(c.c_void_p), c.c_uint, c.c_int], c.byref(context), 0, device)
    try:
        source = c.create_string_buffer(Path(args.ptx).read_bytes())
        call('cuModuleLoadData', [c.POINTER(c.c_void_p), c.c_void_p], c.byref(module), source)
        functions = []
        for name in ('float_to_half', 'half_to_float'):
            function = c.c_void_p()
            call('cuModuleGetFunction', [c.POINTER(c.c_void_p), c.c_void_p, c.c_char_p], c.byref(function), module, name.encode())
            functions.append(function)
        rng = random.Random(32)
        values = [0., -0., 1., -1., 1.00048828125, 1.00146484375, 2. ** -24, 2. ** -25, 65504., -65504.,
                  float('inf'), -float('inf'), float('nan')] + [rng.uniform(-100, 100) for _ in range(244)]
        packed = struct.pack(f'<{len(values)}f', *values)
        values = struct.unpack(f'<{len(values)}f', packed)
        for size in (len(packed), len(values) * 2, len(packed)):
            buffer = c.c_uint64()
            call('cuMemAlloc_v2', [c.POINTER(c.c_uint64), c.c_size_t], c.byref(buffer), size)
            buffers.append(buffer)
        source = c.create_string_buffer(packed)
        call('cuMemcpyHtoD_v2', [c.c_uint64, c.c_void_p, c.c_size_t], buffers[0], source, len(packed))
        count = c.c_size_t(len(values))
        for function, input_buffer, output_buffer in zip(functions, buffers, buffers[1:]):
            params = (c.c_void_p * 3)(*[c.cast(c.byref(v), c.c_void_p) for v in (input_buffer, output_buffer, count)])
            call('cuLaunchKernel', [c.c_void_p] + [c.c_uint] * 7 + [c.c_void_p, c.c_void_p, c.c_void_p],
                 function, (len(values) + 255) // 256, 1, 1, 256, 1, 1, 0, None, params, None)
        call('cuCtxSynchronize', [])
        result = c.create_string_buffer(len(packed))
        call('cuMemcpyDtoH_v2', [c.c_void_p, c.c_uint64, c.c_size_t], result, buffers[2], len(packed))
        actual = struct.unpack(f'<{len(values)}f', result.raw)
        expected = struct.unpack(f'<{len(values)}e', struct.pack(f'<{len(values)}e', *values))
        for a, b in zip(actual, expected):
            assert math.isnan(a) if math.isnan(b) else a == b, (a, b)
            if b == 0: assert math.copysign(1, a) == math.copysign(1, b)
        print(f'{len(values)} FP32/FP16 round trips match IEEE half reference on the local GPU, including ties, subnormals, signed zero and nonfinite values.')
    finally:
        for buffer in buffers: call('cuMemFree_v2', [c.c_uint64], buffer)
        if module.value: call('cuModuleUnload', [c.c_void_p], module)
        call('cuCtxDestroy_v2', [c.c_void_p], context)


if __name__ == '__main__':
    main()
