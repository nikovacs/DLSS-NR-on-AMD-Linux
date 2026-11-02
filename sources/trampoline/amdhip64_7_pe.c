/* PE trampoline: Win64 exports → Linux HIP via DlssnrHipBridge (same process).
 * Compiled with clang -target x86_64-pc-windows-msvc (or gnu). */
#include "hip_bridge.h"

#define EXPORT __declspec(dllexport)
#define SYSV __attribute__((sysv_abi))

typedef int BOOL;
typedef unsigned int DWORD;
typedef void *HANDLE;
#define DLL_PROCESS_ATTACH 1
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)

__declspec(dllimport) DWORD __stdcall GetEnvironmentVariableA(const char *name, char *buf, DWORD size);
__declspec(dllimport) int __stdcall __wine_get_unix_env(const char *var, char *buf, unsigned long long size);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char *name, DWORD acc, DWORD share, void *sa,
                                                   DWORD disp, DWORD attr, HANDLE tmpl);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *w, void *ov);
__declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE h);
__declspec(dllimport) HANDLE __stdcall LoadLibraryA(const char *name);
__declspec(dllimport) void *__stdcall GetProcAddress(HANDLE module, const char *name);
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char *name);
__declspec(dllimport) DWORD __stdcall GetModuleFileNameA(HANDLE module, char *buf, DWORD size);
__declspec(dllimport) BOOL __stdcall GetModuleHandleExA(DWORD flags, const char *name, HANDLE *module);
__declspec(dllimport) BOOL __stdcall VirtualProtect(void *addr, unsigned long long size, DWORD prot, DWORD *old);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *addr, unsigned long long size, DWORD type, DWORD prot);
__declspec(dllimport) BOOL __stdcall FlushInstructionCache(HANDLE process, const void *addr, unsigned long long size);

static DlssnrHipBridge *g;

/* No C runtime (/nodefaultlib): the compiler may still emit memcpy/memset for byte loops.
 * Volatile accesses keep these from being turned back into calls to themselves.
 * (Only in the PE build; host-side test harnesses link the C library.) */
#ifdef _WIN32
void *memcpy(void *dst, const void *src, unsigned long long n) {
    volatile unsigned char *d = dst; const volatile unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int c, unsigned long long n) {
    volatile unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
#endif

static unsigned long long parse_hex_ptr(const char *s) {
    unsigned long long v = 0;
    if (!s)
        return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            break;
    }
    return v;
}

static int bridge_ok(void) {
    return g && g->magic == DLSSNR_HIP_MAGIC;
}

static HANDLE g_self;
/* dlssnr_pe.log beside this DLL (the game folder): visible from the host, unlike the
 * container's private /tmp under Flatpak Steam. Falls back to Z:\\tmp. */
static void pe_log(const char *s, unsigned n) {
    char path[600];
    DWORD len = g_self ? GetModuleFileNameA(g_self, path, sizeof(path) - 16) : 0;
    while (len && path[len - 1] != '\\' && path[len - 1] != '/') --len;
    if (len) {
        const char *name = "dlssnr_pe.log";
        for (unsigned i = 0; name[i]; ++i) path[len++] = name[i];
        path[len] = 0;
    } else {
        const char *fallback = "Z:\\tmp\\dlssnr-pe.log";
        for (len = 0; fallback[len]; ++len) path[len] = fallback[len];
        path[len] = 0;
    }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    WriteFile(h, s, n, &w, 0);
    CloseHandle(h);
}

static void init_bridge(void) {
    char buf[64];
    char msg[160];
    unsigned i;
    buf[0] = 0;
    /* Live Unix environ (LD_PRELOAD setenv). Wine PE env is a stale snapshot. */
    if (__wine_get_unix_env(DLSSNR_HIP_ENV, buf, sizeof(buf)) != 0 || !buf[0]) {
        DWORD n = GetEnvironmentVariableA(DLSSNR_HIP_ENV, buf, (DWORD)sizeof(buf));
        if (n == 0 || n >= sizeof(buf))
            buf[0] = 0;
    }
    if (buf[0])
        g = (DlssnrHipBridge *)(unsigned long long)parse_hex_ptr(buf);
    /* tiny hex dump into pe log */
    msg[0] = 'e'; msg[1] = 'n'; msg[2] = 'v'; msg[3] = '=';
    for (i = 0; i < 48 && buf[i]; i++)
        msg[4 + i] = buf[i];
    msg[4 + i] = ' ';
    msg[5 + i] = 'o';
    msg[6 + i] = 'k';
    msg[7 + i] = '=';
    msg[8 + i] = (char)('0' + (bridge_ok() ? 1 : 0));
    msg[9 + i] = '\n';
    pe_log(msg, 10 + i);
}


/* Upstream 0.8.0 (NeuralHub host) under Wine: loader-lock deadlock guard.
 *
 * The host detours LoadLibraryExW twice (its FSR/FFX and XeSS interceptors). Each detour
 * calls the original and then inspects the newly loaded module under the host's own
 * synchronisation. When a DLL's startup code (PROCESS_ATTACH, i.e. with the loader lock
 * held) calls LoadLibraryExW, the detour runs under the loader lock and waits for the host;
 * meanwhile the host thread, holding that, calls module lookups (GetModuleHandle*, ...).
 * On Windows 8+ those lookups do not take the loader lock; Wine's do, so both threads wait
 * forever (seen in Hogwarts Legacy: Wwise plugin DllMain vs the host's interceptor setup).
 *
 * Guard: when the calling thread already owns the loader lock, the detour is skipped and
 * the original LoadLibraryExW (the pointer the host saved) is called directly. All other
 * calls, including the game loading its FSR/XeSS DLLs, run the host's detour unchanged.
 * Applied in memory only, to the exact pinned build: every patch site's bytes must match,
 * otherwise nothing is changed. The host's DLL file is never modified. */
#define NH080_SIZE_OF_IMAGE 0x06139000u
struct nh_site {
    u64 detour_rva, orig_slot_rva;
    unsigned len;                /* prologue bytes moved (whole instructions, position-independent) */
    unsigned char prologue[16];
    unsigned char *cont;         /* moved prologue + jump back into the detour */
    void **orig_slot;
};
static struct nh_site nh_sites[2] = {
    /* XeSS interceptor's LoadLibraryExW detour */
    {0x57500, 0x1a03968, 16, {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xec,0x48}, 0, 0},
    /* FSR/FFX interceptor's LoadLibraryExW detour */
    {0x1b1a0, 0x1a01d48, 14, {0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xec,0x50}, 0, 0},
};
typedef void *(__stdcall *load_library_ex_w)(const void *name, void *file, DWORD flags);

static int loader_lock_held(void) {
    unsigned char *peb, *teb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
    unsigned char *lock = *(unsigned char **)(peb + 0x110);          /* PEB->LoaderLock */
    if (!lock) return 0;
    return *(void **)(lock + 0x10) == *(void **)(teb + 0x48);       /* OwningThread == ClientId.UniqueThread */
}
static void *__stdcall nh_guard_xess(const void *name, void *file, DWORD flags) {
    if (loader_lock_held()) return ((load_library_ex_w)*nh_sites[0].orig_slot)(name, file, flags);
    return ((load_library_ex_w)(void *)nh_sites[0].cont)(name, file, flags);
}
static void *__stdcall nh_guard_ffx(const void *name, void *file, DWORD flags) {
    if (loader_lock_held()) return ((load_library_ex_w)*nh_sites[1].orig_slot)(name, file, flags);
    return ((load_library_ex_w)(void *)nh_sites[1].cont)(name, file, flags);
}
static void put_jump(unsigned char *at, const void *target) {   /* jmp qword ptr [rip+0]; dq target (14 bytes) */
    unsigned long long t = (unsigned long long)target;
    at[0] = 0xff; at[1] = 0x25; at[2] = at[3] = at[4] = at[5] = 0;
    for (int i = 0; i < 8; ++i) at[6 + i] = (unsigned char)(t >> (8 * i));
}
static const char *install_nh080_guard(void) {
    unsigned char *base = (unsigned char *)GetModuleHandleA("neuralhub_host.dll");
    if (!base) return "loaderlock-guard: no neuralhub_host.dll";
    unsigned char *nt = base + *(unsigned int *)(base + 0x3c);
    if (*(unsigned int *)nt != 0x4550 || *(unsigned int *)(nt + 0x50) != NH080_SIZE_OF_IMAGE)
        return "loaderlock-guard: other host build, not applied";
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned k = 0; k < nh_sites[i].len; ++k)
            if (base[nh_sites[i].detour_rva + k] != nh_sites[i].prologue[k])
                return "loaderlock-guard: detour bytes differ (already patched or other build), not applied";
    unsigned char *mem = VirtualAlloc(0, 4096, 0x3000 /* MEM_COMMIT|MEM_RESERVE */, 0x40 /* PAGE_EXECUTE_READWRITE */);
    if (!mem) return "loaderlock-guard: VirtualAlloc failed";
    void *stubs[2] = {(void *)nh_guard_xess, (void *)nh_guard_ffx};
    for (unsigned i = 0; i < 2; ++i) {
        struct nh_site *site = &nh_sites[i];
        unsigned char *detour = base + site->detour_rva;
        site->orig_slot = (void **)(base + site->orig_slot_rva);
        site->cont = mem + 64 * i;
        for (unsigned k = 0; k < site->len; ++k) site->cont[k] = site->prologue[k];
        put_jump(site->cont + site->len, detour + site->len);
        DWORD old = 0;
        if (!VirtualProtect(detour, 16, 0x40, &old)) return "loaderlock-guard: VirtualProtect failed";
        put_jump(detour, stubs[i]);
        for (unsigned k = 14; k < site->len; ++k) detour[k] = 0x90;   /* nop the moved tail */
        VirtualProtect(detour, 16, old, &old);
    }
    FlushInstructionCache((HANDLE)(long long)-1, base, NH080_SIZE_OF_IMAGE);
    /* The host's detours now jump into this DLL: never let it unload (AMDNR may free a
     * probe copy). GET_MODULE_HANDLE_EX_FLAG_PIN | FROM_ADDRESS. */
    HANDLE self = 0;
    GetModuleHandleExA(0x1 | 0x4, (const char *)(void *)nh_guard_xess, &self);
    return "loaderlock-guard: applied to the 0.8.0 host (2 LoadLibraryExW detours); module pinned";
}

BOOL __stdcall DllMain(void *h, DWORD reason, void *r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        init_bridge();
        const char *g = install_nh080_guard();
        unsigned n = 0;
        while (g[n]) ++n;
        pe_log(g, n);
    }
    return 1;
}

#define HIP_ERROR_NOT_INITIALIZED 3
#define HIP_ERROR_INVALID_VALUE 1
#define HIP_SUCCESS 0

EXPORT void *__hipRegisterFatBinary(void *data) {
    if (!bridge_ok() || !g->p___hipRegisterFatBinary)
        return 0;
    return ((void *SYSV (*)(void *))g->p___hipRegisterFatBinary)(data);
}

EXPORT void __hipUnregisterFatBinary(void **modules) {
    if (!bridge_ok() || !g->p___hipUnregisterFatBinary)
        return;
    ((void SYSV (*)(void **))g->p___hipUnregisterFatBinary)(modules);
}

EXPORT void __hipRegisterFunction(void **modules, const void *hostFunction, char *deviceFunction,
                                  const char *deviceName, unsigned int threadLimit, void *tid,
                                  void *bid, void *blockDim, void *gridDim, int *wSize) {
    if (!bridge_ok() || !g->p___hipRegisterFunction)
        return;
    ((void SYSV (*)(void **, const void *, char *, const char *, unsigned int, void *, void *, void *,
                    void *, int *))g->p___hipRegisterFunction)(
        modules, hostFunction, deviceFunction, deviceName, threadLimit, tid, bid, blockDim, gridDim,
        wSize);
}

EXPORT void __hipRegisterVar(void **modules, void *var, char *hostVar, char *deviceVar, int ext,
                             u64 size, int constant, int global) {
    if (!bridge_ok() || !g->p___hipRegisterVar)
        return;
    ((void SYSV (*)(void **, void *, char *, char *, int, u64, int, int))g->p___hipRegisterVar)(
        modules, var, hostVar, deviceVar, ext, size, constant, global);
}

EXPORT int __hipPushCallConfiguration(dim3_t gridDim, dim3_t blockDim, u64 sharedMem,
                                      void *stream) {
    if (!bridge_ok() || !g->p___hipPushCallConfiguration)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(dim3_t, dim3_t, u64, void *))g->p___hipPushCallConfiguration)(
        gridDim, blockDim, sharedMem, stream);
}

EXPORT int __hipPopCallConfiguration(dim3_t *gridDim, dim3_t *blockDim, u64 *sharedMem,
                                     void **stream) {
    if (!bridge_ok() || !g->p___hipPopCallConfiguration)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(dim3_t *, dim3_t *, u64 *, void **))g->p___hipPopCallConfiguration)(
        gridDim, blockDim, sharedMem, stream);
}

EXPORT int hipGetDeviceCount(int *count) {
    if (!bridge_ok() || !g->p_hipGetDeviceCount)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(int *))g->p_hipGetDeviceCount)(count);
}

EXPORT int hipGetDevice(int *device) {
    if (!bridge_ok() || !g->p_hipGetDevice) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(int *))g->p_hipGetDevice)(device);
}

EXPORT int hipOccupancyMaxActiveBlocksPerMultiprocessor(int *blocks,
        const void *function_address, int block_size, u64 dynamic_shared_bytes) {
    if (!bridge_ok() || !g->p_hipOccupancyMaxActiveBlocksPerMultiprocessor)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(int *, const void *, int, u64))
        g->p_hipOccupancyMaxActiveBlocksPerMultiprocessor)(
        blocks, function_address, block_size, dynamic_shared_bytes);
}

EXPORT int hipSetDevice(int device) {
    if (!bridge_ok() || !g->p_hipSetDevice)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(int))g->p_hipSetDevice)(device);
}

/* Linux HIP has no Windows adapter identity: its hipDeviceProp_tR0600 LUID (offset 272)
 * is zero. Hosts that pair HIP with D3D12 by LUID (AMDNR) then find no device. Wine
 * assigns the D3D12/DXGI adapter LUIDs itself; with exactly one adapter there is only
 * one possible pairing, so report that LUID. Several adapters: leave it zero (the host
 * reports the mismatch) rather than guess. */
typedef struct { unsigned int hAdapter; unsigned int lo; int hi; unsigned int sources; BOOL precise; } KmtAdapterInfo;
typedef struct { unsigned int count; KmtAdapterInfo *adapters; } KmtEnumAdapters2;

static int single_adapter_luid(unsigned char luid[8]) {
    HANDLE gdi = LoadLibraryA("gdi32.dll");
    long (__stdcall *enum2)(KmtEnumAdapters2 *) =
        gdi ? (long (__stdcall *)(KmtEnumAdapters2 *))GetProcAddress(gdi, "D3DKMTEnumAdapters2") : 0;
    KmtAdapterInfo info[8];
    KmtEnumAdapters2 query = {8, info};
    if (!enum2 || enum2(&query) != 0 || query.count != 1)
        return 0;
    __builtin_memcpy(luid, &info[0].lo, 4);
    __builtin_memcpy(luid + 4, &info[0].hi, 4);
    return 1;
}

EXPORT int hipGetDevicePropertiesR0600(void *prop, int deviceId) {
    if (!bridge_ok() || !g->p_hipGetDevicePropertiesR0600)
        return HIP_ERROR_NOT_INITIALIZED;
    int rc = ((int SYSV (*)(void *, int))g->p_hipGetDevicePropertiesR0600)(prop, deviceId);
    unsigned char *p = (unsigned char *)prop, luid[8], any = 0;
    for (int i = 0; p && i < 8; ++i)
        any |= p[272 + i];
    if (!rc && p && !any && single_adapter_luid(luid)) {
        unsigned int node_mask = 1;
        __builtin_memcpy(p + 272, luid, 8);
        __builtin_memcpy(p + 280, &node_mask, 4);
    }
    return rc;
}

/* Daniel's runtime never calls hipInit; AMDNR's HIP probe does. Linux HIP initialises
 * on first use, which hipGetDeviceCount performs through the bridge (no ABI change). */
EXPORT int hipInit(unsigned int flags) {
    int count = 0, rc;
    if (flags)
        return 1; /* hipErrorInvalidValue: HIP accepts only 0 */
    rc = hipGetDeviceCount(&count);
    return rc ? rc : count ? 0 : 100; /* hipErrorNoDevice */
}

EXPORT int hipDriverGetVersion(int *driverVersion) {
    if (!bridge_ok() || !g->p_hipDriverGetVersion)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(int *))g->p_hipDriverGetVersion)(driverVersion);
}

EXPORT int hipRuntimeGetVersion(int *runtimeVersion) {
    if (!bridge_ok() || !g->p_hipRuntimeGetVersion)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(int *))g->p_hipRuntimeGetVersion)(runtimeVersion);
}

EXPORT int hipMalloc(void **ptr, u64 size) {
    if (!bridge_ok() || !g->p_hipMalloc)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **, u64))g->p_hipMalloc)(ptr, size);
}

EXPORT int hipFree(void *ptr) {
    if (!bridge_ok() || !g->p_hipFree)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *))g->p_hipFree)(ptr);
}

EXPORT int hipMemcpy(void *dst, const void *src, u64 sizeBytes, int kind) {
    if (!bridge_ok() || !g->p_hipMemcpy)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *, const void *, u64, int))g->p_hipMemcpy)(dst, src, sizeBytes,
                                                                                    kind);
}

EXPORT int hipMemcpyAsync(void *dst, const void *src, u64 sizeBytes, int kind, void *stream) {
    if (!bridge_ok() || !g->p_hipMemcpyAsync)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *, const void *, u64, int, void *))g->p_hipMemcpyAsync)(
        dst, src, sizeBytes, kind, stream);
}

EXPORT int hipMemcpyToSymbol(const void *symbol, const void *src, u64 sizeBytes,
                             u64 offset, int kind) {
    if (!bridge_ok() || !g->p_hipMemcpyToSymbol)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(const void *, const void *, u64, u64, int))
                g->p_hipMemcpyToSymbol)(symbol, src, sizeBytes, offset, kind);
}

EXPORT int hipMemset(void *dst, int value, u64 sizeBytes) {
    if (!bridge_ok() || !g->p_hipMemset)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *, int, u64))g->p_hipMemset)(dst, value, sizeBytes);
}

EXPORT int hipMemsetAsync(void *dst, int value, u64 sizeBytes, void *stream) {
    if (!bridge_ok() || !g->p_hipMemsetAsync)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *, int, u64, void *))g->p_hipMemsetAsync)(dst, value, sizeBytes,
                                                                                   stream);
}

EXPORT int hipLaunchKernel(const void *function_address, dim3_t numBlocks, dim3_t dimBlocks, void **args,
                           u64 sharedMemBytes, void *stream) {
    if (!bridge_ok() || !g->p_hipLaunchKernel)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(const void *, dim3_t, dim3_t, void **, u64, void *))
                g->p_hipLaunchKernel)(function_address, numBlocks, dimBlocks, args, sharedMemBytes,
                                      stream);
}

EXPORT int hipDeviceSynchronize(void) {
    if (!bridge_ok() || !g->p_hipDeviceSynchronize)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void))g->p_hipDeviceSynchronize)();
}

EXPORT int hipGetLastError(void) {
    if (!bridge_ok() || !g->p_hipGetLastError)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void))g->p_hipGetLastError)();
}

EXPORT const char *hipGetErrorString(int error) {
    if (!bridge_ok() || !g->p_hipGetErrorString)
        return "hip bridge not initialized";
    return ((const char *SYSV (*)(int))g->p_hipGetErrorString)(error);
}

EXPORT int hipEventCreate(void **event) {
    if (!bridge_ok() || !g->p_hipEventCreate)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **))g->p_hipEventCreate)(event);
}

EXPORT int hipEventCreateWithFlags(void **event, unsigned int flags) {
    if (!bridge_ok() || !g->p_hipEventCreateWithFlags) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **, unsigned int))g->p_hipEventCreateWithFlags)(event, flags);
}

EXPORT int hipEventQuery(void *event) {
    if (!bridge_ok() || !g->p_hipEventQuery) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *))g->p_hipEventQuery)(event);
}

EXPORT int hipStreamCreateWithFlags(void **stream, unsigned int flags) {
    if (!bridge_ok() || !g->p_hipStreamCreateWithFlags) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **, unsigned int))g->p_hipStreamCreateWithFlags)(stream, flags);
}

EXPORT int hipDeviceGetStreamPriorityRange(int *least, int *greatest) {
    if (!bridge_ok() || !g->p_hipDeviceGetStreamPriorityRange) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(int *, int *))g->p_hipDeviceGetStreamPriorityRange)(least, greatest);
}

EXPORT int hipStreamCreateWithPriority(void **stream, unsigned int flags, int priority) {
    if (!bridge_ok() || !g->p_hipStreamCreateWithPriority) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **, unsigned int, int))g->p_hipStreamCreateWithPriority)(stream, flags, priority);
}

EXPORT int hipStreamDestroy(void *stream) {
    if (!bridge_ok() || !g->p_hipStreamDestroy) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *))g->p_hipStreamDestroy)(stream);
}

EXPORT int hipHostMalloc(void **ptr, u64 size, unsigned int flags) {
    if (!bridge_ok() || !g->p_hipHostMalloc) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **, u64, unsigned int))g->p_hipHostMalloc)(ptr, size, flags);
}

EXPORT int hipHostGetDevicePointer(void **devPtr, void *hostPtr, unsigned int flags) {
    if (!bridge_ok() || !g->p_hipHostGetDevicePointer) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **, void *, unsigned int))g->p_hipHostGetDevicePointer)(devPtr, hostPtr, flags);
}

/* ABI 6: upstream 0.8.0 resolves these by name at first use. */
EXPORT int hipCreateTextureObject(void **tex, const void *res_desc, const void *tex_desc, const void *view_desc) {
    if (!bridge_ok() || !g->p_hipCreateTextureObject) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **tex, const void *res_desc, const void *tex_desc, const void *view_desc))g->p_hipCreateTextureObject)(tex, res_desc, tex_desc, view_desc);
}

EXPORT int hipDestroyTextureObject(void *tex) {
    if (!bridge_ok() || !g->p_hipDestroyTextureObject) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *tex))g->p_hipDestroyTextureObject)(tex);
}

EXPORT int hipEventDestroy(void *event) {
    if (!bridge_ok() || !g->p_hipEventDestroy) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *event))g->p_hipEventDestroy)(event);
}

EXPORT int hipHostFree(void *ptr) {
    if (!bridge_ok() || !g->p_hipHostFree) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *ptr))g->p_hipHostFree)(ptr);
}

EXPORT int hipMallocPitch(void **ptr, u64 *pitch, u64 width, u64 height) {
    if (!bridge_ok() || !g->p_hipMallocPitch) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **ptr, u64 *pitch, u64 width, u64 height))g->p_hipMallocPitch)(ptr, pitch, width, height);
}

EXPORT int hipMemcpy2D(void *dst, u64 dpitch, const void *src, u64 spitch, u64 width, u64 height, int kind) {
    if (!bridge_ok() || !g->p_hipMemcpy2D) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *dst, u64 dpitch, const void *src, u64 spitch, u64 width, u64 height, int kind))g->p_hipMemcpy2D)(dst, dpitch, src, spitch, width, height, kind);
}

EXPORT int hipMemcpyFromSymbol(void *dst, const void *symbol, u64 bytes, u64 offset, int kind) {
    if (!bridge_ok() || !g->p_hipMemcpyFromSymbol) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *dst, const void *symbol, u64 bytes, u64 offset, int kind))g->p_hipMemcpyFromSymbol)(dst, symbol, bytes, offset, kind);
}

EXPORT int hipMemset2D(void *dst, u64 pitch, int value, u64 width, u64 height) {
    if (!bridge_ok() || !g->p_hipMemset2D) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *dst, u64 pitch, int value, u64 width, u64 height))g->p_hipMemset2D)(dst, pitch, value, width, height);
}

EXPORT int hipMemset2DAsync(void *dst, u64 pitch, int value, u64 width, u64 height, void *stream) {
    if (!bridge_ok() || !g->p_hipMemset2DAsync) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *dst, u64 pitch, int value, u64 width, u64 height, void *stream))g->p_hipMemset2DAsync)(dst, pitch, value, width, height, stream);
}

EXPORT int hipMemsetD16Async(void *dst, unsigned short value, u64 count, void *stream) {
    if (!bridge_ok() || !g->p_hipMemsetD16Async) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *dst, unsigned short value, u64 count, void *stream))g->p_hipMemsetD16Async)(dst, value, count, stream);
}

EXPORT int hipStreamCreate(void **stream) {
    if (!bridge_ok() || !g->p_hipStreamCreate) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void **stream))g->p_hipStreamCreate)(stream);
}


EXPORT int hipStreamSynchronize(void *stream) {
    if (!bridge_ok() || !g->p_hipStreamSynchronize) return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *))g->p_hipStreamSynchronize)(stream);
}

EXPORT int hipEventRecord(void *event, void *stream) {
    if (!bridge_ok() || !g->p_hipEventRecord)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *, void *))g->p_hipEventRecord)(event, stream);
}

EXPORT int hipEventSynchronize(void *event) {
    if (!bridge_ok() || !g->p_hipEventSynchronize)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *))g->p_hipEventSynchronize)(event);
}

EXPORT int hipEventElapsedTime(float *ms, void *start, void *stop) {
    if (!bridge_ok() || !g->p_hipEventElapsedTime)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(float *, void *, void *))g->p_hipEventElapsedTime)(ms, start, stop);
}

EXPORT int hipImportExternalMemory(void **extMem_out, const void *memHandleDesc) {
    /* Windows D3D12/NT handles cannot be imported by Linux HIP. Let the mod fall back to readback. */
    if (!bridge_ok() || !g->p_hipImportExternalMemory)
        return HIP_ERROR_INVALID_VALUE;
    return ((int SYSV (*)(void **, const void *))g->p_hipImportExternalMemory)(extMem_out, memHandleDesc);
}

EXPORT int hipExternalMemoryGetMappedBuffer(void **devPtr, void *extMem, const void *bufferDesc) {
    if (!bridge_ok() || !g->p_hipExternalMemoryGetMappedBuffer)
        return HIP_ERROR_INVALID_VALUE;
    return ((int SYSV (*)(void **, void *, const void *))g->p_hipExternalMemoryGetMappedBuffer)(
        devPtr, extMem, bufferDesc);
}

EXPORT int hipDestroyExternalMemory(void *extMem) {
    if (!bridge_ok() || !g->p_hipDestroyExternalMemory)
        return HIP_ERROR_INVALID_VALUE;
    return ((int SYSV (*)(void *))g->p_hipDestroyExternalMemory)(extMem);
}
