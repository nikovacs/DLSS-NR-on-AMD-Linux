// Layer 1 test: does native Linux HIP work on this GPU, and what does it report?
// Mirrors the check in danielblnc's runtime (version.dll 0.6.0 @ 0x18000d4cb):
//   supported = hipSetDevice()==0 && gcnArchName is exactly 7 chars of gfx103x/gfx11xx/gfx120x
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstring>
#include <cstddef>

#define CHECK(x) do { hipError_t e = (x); if (e != hipSuccess) { \
    printf("FAIL %s -> %d (%s)\n", #x, e, hipGetErrorString(e)); return 1; } } while (0)

// The kernel: each GPU thread writes out[i] = a*i + b. One launch, many threads in parallel.
__global__ void saxpy_like(float *out, float a, float b, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = a * i + b;
}

int main() {
    int count = 0;
    CHECK(hipGetDeviceCount(&count));
    printf("devices: %d\n", count);

    int rc = hipSetDevice(0);
    printf("hipSetDevice(0) = %d\n", rc);

    hipDeviceProp_t p;
    CHECK(hipGetDeviceProperties(&p, 0));
    printf("name        = '%s'\n", p.name);
    printf("gcnArchName = '%s' (strlen %zu)\n", p.gcnArchName, strlen(p.gcnArchName));
    printf("struct offsets: name @0x%zx, gcnArchName @0x%zx, sizeof %zu\n",
           offsetof(hipDeviceProp_t, name), offsetof(hipDeviceProp_t, gcnArchName), sizeof(p));
    printf("CUs=%d  VRAM=%.0f MB  warpSize=%d\n", p.multiProcessorCount,
           p.totalGlobalMem / 1048576.0, p.warpSize);

    // Reproduce Daniel's check.
    const char *a = p.gcnArchName;
    bool len7  = strlen(a) == 7;
    bool rdna2 = !strncmp(a, "gfx103", 6) && len7;
    bool rdna3 = !strncmp(a, "gfx11", 5) && len7;
    bool rdna4 = !strncmp(a, "gfx120", 6) && len7;
    printf("Daniel's check would say: %s\n",
           (rc == 0 && (rdna2 || rdna3 || rdna4)) ? "SUPPORTED" : "UNSUPPORTED");

    // Run a real kernel on the GPU and verify the result on the CPU.
    const int n = 1 << 20;
    float *d = nullptr, *h = new float[n];
    CHECK(hipMalloc(&d, n * sizeof(float)));
    saxpy_like<<<(n + 255) / 256, 256>>>(d, 2.0f, 1.0f, n);
    CHECK(hipGetLastError());
    CHECK(hipMemcpy(h, d, n * sizeof(float), hipMemcpyDeviceToHost));
    int bad = 0;
    for (int i = 0; i < n; i++) if (h[i] != 2.0f * i + 1.0f) bad++;
    printf("kernel: %d threads, %d wrong -> %s\n", n, bad, bad ? "FAIL" : "OK");
    hipFree(d);
    delete[] h;
    return bad != 0;
}
