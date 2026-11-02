#include <hip/hip_runtime.h>
// extern "C" so the kernel's symbol name is exactly "probe_fill" (no C++ name mangling).
extern "C" __global__ void probe_fill(float *out, float a, float b, int n, unsigned *flag) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = a * i + b;
    if (i == 0) *flag = 0xC0FFEE;   // write through the mapped host pointer: the CPU sees it directly
}
