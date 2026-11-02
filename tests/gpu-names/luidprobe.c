/* Adapter identity as seen by DXGI, D3DKMT and HIP (through amdhip64_7.dll beside this exe).
 * AMDNR matches the HIP device whose hipDeviceProp_tR0600 LUID (offset 272) equals the
 * D3D12 adapter LUID. Output: stdout, or the file named by LUIDPROBE_OUT. */
#define INITGUID
#include <windows.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>

typedef struct { UINT hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPrecisePresentRegionsPreferred; } KMT_ADAPTERINFO;
typedef struct { ULONG NumAdapters; KMT_ADAPTERINFO *pAdapters; } KMT_ENUMADAPTERS2;
typedef LONG (WINAPI *EnumAdapters2Fn)(KMT_ENUMADAPTERS2 *);

static FILE *out;

static void dxgi(void) {
    IDXGIFactory1 *f = NULL; IDXGIAdapter1 *a; DXGI_ADAPTER_DESC1 d; char name[128];
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) { fprintf(out, "dxgi: factory failed\n"); return; }
    for (UINT i = 0; f->lpVtbl->EnumAdapters1(f, i, &a) == S_OK; ++i) {
        a->lpVtbl->GetDesc1(a, &d);
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
        fprintf(out, "dxgi   %u: \"%s\" pci=%04x:%04x luid=%08lx-%08lx flags=%u\n", i, name, d.VendorId, d.DeviceId,
                (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart, d.Flags);
        a->lpVtbl->Release(a);
    }
    f->lpVtbl->Release(f);
}

static void kmt(void) {
    EnumAdapters2Fn e = (EnumAdapters2Fn)GetProcAddress(LoadLibraryW(L"gdi32.dll"), "D3DKMTEnumAdapters2");
    KMT_ADAPTERINFO info[16]; KMT_ENUMADAPTERS2 q = {16, info};
    if (!e) { fprintf(out, "kmt: D3DKMTEnumAdapters2 missing\n"); return; }
    LONG s = e(&q);
    fprintf(out, "kmt: status=0x%lx adapters=%lu\n", (unsigned long)s, q.NumAdapters);
    for (ULONG i = 0; !s && i < q.NumAdapters; ++i)
        fprintf(out, "kmt    %lu: luid=%08lx-%08lx sources=%lu\n", i, (unsigned long)info[i].AdapterLuid.HighPart,
                (unsigned long)info[i].AdapterLuid.LowPart, info[i].NumOfSources);
}

static void hip(void) {
    HMODULE h = LoadLibraryW(L"amdhip64_7.dll");
    if (!h) { fprintf(out, "hip: amdhip64_7.dll not loadable (%lu)\n", GetLastError()); return; }
    int (*init)(unsigned) = (void *)GetProcAddress(h, "hipInit");
    int (*count)(int *) = (void *)GetProcAddress(h, "hipGetDeviceCount");
    int (*props)(void *, int) = (void *)GetProcAddress(h, "hipGetDevicePropertiesR0600");
    if (!init || !count || !props) { fprintf(out, "hip: exports missing\n"); return; }
    int n = 0, rc = init(0);
    fprintf(out, "hip: init=%d count=%d n=%d\n", rc, count(&n), n);
    for (int i = 0; i < n; ++i) {
        static unsigned char p[8192] __attribute__((aligned(16)));
        memset(p, 0xcc, sizeof p);
        rc = props(p, i);
        uint32_t lo, hi, mask; memcpy(&lo, p + 272, 4); memcpy(&hi, p + 276, 4); memcpy(&mask, p + 280, 4);
        fprintf(out, "hip    %d: rc=%d \"%.64s\" luid=%08x-%08x nodemask=%08x uuid=", i, rc, (char *)p, hi, lo, mask);
        for (int k = 0; k < 16; ++k) fprintf(out, "%02x", p[256 + k]);
        fprintf(out, "\n");
    }
}

int main(void) {
    char path[MAX_PATH];
    out = GetEnvironmentVariableA("LUIDPROBE_OUT", path, sizeof path) ? fopen(path, "w") : stdout;
    if (!out) out = stdout;
    dxgi(); kmt(); hip();
    fclose(out);
    return 0;
}
