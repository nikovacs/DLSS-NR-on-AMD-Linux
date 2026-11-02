/* Layer-2 probe: a Windows program that drives HIP exactly the way danielblnc's
 * runtime does, through whatever amdhip64_7.dll the loader finds (under Proton:
 * the bridge trampoline). Build: x86_64-w64-mingw32-clang probe.c -o probe.exe
 *
 * Checks, in order:
 *   1. amdhip64_7.dll loads and exports what upstream 0.6.0 needs
 *   2. device + properties pass upstream's support check (hipSetDevice ok, 7-char gfx103x/gfx11xx/gfx120x)
 *   3. 0.6.0's mapped host flag (hipHostMalloc + hipHostGetDevicePointer)
 *   4. fat-binary registration + a real kernel launch, verified on the CPU
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "kernel_bundle.h" /* xxd -i kernel.hsaco: unsigned char kernel_hsaco[] */

typedef struct { unsigned x, y, z; } dim3;
/* hipDeviceProp_tR0600: 1472 bytes, name @0, gcnArchName @0x488 (matches upstream's memset 0x5c0). */
typedef struct { unsigned char raw[0x5c0]; } props_t;
/* What clang emits for HIP: __hipRegisterFatBinary(&wrapper). */
typedef struct { unsigned magic, version; const void *binary; void *reserved; } fatbin_wrapper;

#define LOAD(name) name##_t name = (name##_t)GetProcAddress(hip, #name); \
    if (!name) { printf("FAIL  export missing: %s\n", #name); fails++; }

typedef int (*hipGetDeviceCount_t)(int *);
typedef int (*hipSetDevice_t)(int);
typedef int (*hipGetDevicePropertiesR0600_t)(void *, int);
typedef int (*hipRuntimeGetVersion_t)(int *);
typedef int (*hipMalloc_t)(void **, unsigned long long);
typedef int (*hipFree_t)(void *);
typedef int (*hipMemcpy_t)(void *, const void *, unsigned long long, int);
typedef int (*hipHostMalloc_t)(void **, unsigned long long, unsigned);
typedef int (*hipHostGetDevicePointer_t)(void **, void *, unsigned);
typedef int (*hipLaunchKernel_t)(const void *, dim3, dim3, void **, unsigned long long, void *);
typedef int (*hipDeviceSynchronize_t)(void);
typedef const char *(*hipGetErrorString_t)(int);
typedef void **(*__hipRegisterFatBinary_t)(const void *);
typedef void (*__hipRegisterFunction_t)(void **, const void *, char *, const char *, unsigned,
                                        void *, void *, void *, void *, int *);

static char host_stub; /* the "host function" address HIP uses as the kernel's key */

int main(void) {
    int fails = 0, rc;
    /* Proton's launcher has no console: optionally send output to a file. */
    const char *out = getenv("PROBE_OUT");
    if (out && *out) freopen(out, "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE hip = LoadLibraryA("amdhip64_7.dll");
    if (!hip) { printf("FAIL  LoadLibrary(amdhip64_7.dll) error %lu\n", GetLastError()); return 1; }
    char path[MAX_PATH]; GetModuleFileNameA(hip, path, sizeof path);
    printf("OK    loaded %s\n", path);

    LOAD(hipGetDeviceCount) LOAD(hipSetDevice) LOAD(hipGetDevicePropertiesR0600)
    LOAD(hipRuntimeGetVersion) LOAD(hipMalloc) LOAD(hipFree) LOAD(hipMemcpy)
    LOAD(hipHostMalloc) LOAD(hipHostGetDevicePointer) LOAD(hipLaunchKernel)
    LOAD(hipDeviceSynchronize) LOAD(hipGetErrorString) LOAD(__hipRegisterFatBinary)
    LOAD(__hipRegisterFunction)
    if (fails) return 1;

    int count = -1, ver = -1;
    rc = hipGetDeviceCount(&count);
    printf("%s  hipGetDeviceCount rc=%d count=%d\n", rc ? "FAIL" : "OK  ", rc, count);
    if (rc) { printf("      (%s)\n", hipGetErrorString(rc)); return 1; }
    hipRuntimeGetVersion(&ver);
    printf("      runtime version %d\n", ver);

    int setrc = hipSetDevice(0);
    props_t p; memset(&p, 0, sizeof p);
    rc = hipGetDevicePropertiesR0600(&p, 0);
    const char *name = (const char *)p.raw, *arch = (const char *)p.raw + 0x488;
    size_t alen = strlen(arch);
    int arch_ok = alen == 7 && (!strncmp(arch, "gfx103", 6) || !strncmp(arch, "gfx11", 5) ||
                                !strncmp(arch, "gfx120", 6));
    printf("%s  hipSetDevice rc=%d, props rc=%d\n", setrc || rc ? "FAIL" : "OK  ", setrc, rc);
    printf("      name='%s' gcnArchName='%s' (len %zu)\n", name, arch, alen);
    printf("%s  upstream support check: %s\n", (!setrc && arch_ok) ? "OK  " : "FAIL",
           (!setrc && arch_ok) ? "SUPPORTED" : "UNSUPPORTED");
    if (setrc || !arch_ok) fails++;

    void *hflag = NULL, *dflag = NULL;
    rc = hipHostMalloc(&hflag, 4, 2 /* hipHostMallocMapped */);
    int rc2 = rc ? -1 : hipHostGetDevicePointer(&dflag, hflag, 0);
    printf("%s  mapped host flag: hipHostMalloc rc=%d host=%p, devptr rc=%d dev=%p\n",
           rc || rc2 ? "FAIL" : "OK  ", rc, hflag, rc2, dflag);
    if (rc || rc2) return 1;
    *(volatile unsigned *)hflag = 0;

    static fatbin_wrapper w = {0x48495046 /* "HIPF" */, 1, kernel_hsaco, NULL};
    void **mod = __hipRegisterFatBinary(&w);
    printf("%s  __hipRegisterFatBinary -> %p\n", mod ? "OK  " : "FAIL", (void *)mod);
    if (!mod) return 1;
    __hipRegisterFunction(mod, &host_stub, "probe_fill", "probe_fill", (unsigned)-1, 0, 0, 0, 0, 0);

    const int n = 1 << 20;
    float *dout = NULL;
    rc = hipMalloc((void **)&dout, n * sizeof(float));
    if (rc) { printf("FAIL  hipMalloc rc=%d\n", rc); return 1; }
    float a = 2.0f, b = 1.0f;
    int nn = n;
    void *args[] = {&dout, &a, &b, &nn, &dflag};
    dim3 grid = {(unsigned)((n + 255) / 256), 1, 1}, block = {256, 1, 1};
    rc = hipLaunchKernel(&host_stub, grid, block, args, 0, NULL);
    printf("%s  hipLaunchKernel rc=%d%s%s\n", rc ? "FAIL" : "OK  ", rc, rc ? " " : "",
           rc ? hipGetErrorString(rc) : "");
    if (rc) return 1;
    rc = hipDeviceSynchronize();
    float *h = (float *)malloc(n * sizeof(float));
    int rcm = hipMemcpy(h, dout, n * sizeof(float), 2 /* DeviceToHost */);
    int bad = 0;
    for (int i = 0; i < n; i++) if (h[i] != 2.0f * i + 1.0f) bad++;
    printf("%s  kernel result: sync rc=%d memcpy rc=%d, %d/%d wrong\n",
           (rc || rcm || bad) ? "FAIL" : "OK  ", rc, rcm, bad, n);
    unsigned f = *(volatile unsigned *)hflag;
    printf("%s  GPU wrote mapped flag: 0x%X (want 0xC0FFEE)\n", f == 0xC0FFEE ? "OK  " : "FAIL", f);
    if (rc || rcm || bad || f != 0xC0FFEE) fails++;
    hipFree(dout);

    printf("\n%s\n", fails ? "RESULT: FAIL" : "RESULT: ALL PASS");
    return fails ? 1 : 0;
}
