/* gpunames: print how each graphics API names the GPUs in this environment.
 *
 * Windows build (run under Proton/Wine):  x86_64-w64-mingw32-clang -O2 gpunames.c -o gpunames.exe -ldxgi -lole32
 * Output goes to stdout, or to the file named by GPUNAMES_OUT (Proton has no console).
 *
 * Why: the DLSS-NR Linux installer pinned the game's GPU with DXVK/VKD3D_FILTER_DEVICE_NAME
 * set to ROCm's name. Mesa's Vulkan name differs between host and Flatpak runtimes, so the
 * filter can match nothing ("No adapters found"). PCI vendor:device is the stable identity.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* --- minimal Vulkan declarations (layouts from the Vulkan spec) --- */
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef int VkResult;
typedef struct { int sType; const void *pNext; const char *pApplicationName; uint32_t appVer;
                 const char *pEngineName; uint32_t engVer; uint32_t apiVersion; } VkApplicationInfo;
typedef struct { int sType; const void *pNext; uint32_t flags; const VkApplicationInfo *app;
                 uint32_t layers; const char *const *layerNames; uint32_t exts;
                 const char *const *extNames; } VkInstanceCreateInfo;
typedef struct { int sType; void *pNext; uint8_t props[4096]; } VkPhysicalDeviceProperties2; /* props: VkPhysicalDeviceProperties */
typedef struct { int sType; void *pNext; uint32_t driverID; char driverName[256]; char driverInfo[256];
                 uint8_t conformance[4]; } VkPhysicalDeviceDriverProperties;
typedef struct { int sType; void *pNext; uint8_t deviceUUID[16], driverUUID[16], deviceLUID[8];
                 uint32_t nodeMask, luidValid; } VkPhysicalDeviceIDProperties;
#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 1000059001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES 1000071004
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES 1000196000
typedef VkResult (__stdcall *PFN_vkCreateInstance)(const VkInstanceCreateInfo *, const void *, VkInstance *);
typedef VkResult (__stdcall *PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void (__stdcall *PFN_vkGetPhysicalDeviceProperties2)(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
typedef void *(__stdcall *PFN_vkGetInstanceProcAddr)(VkInstance, const char *);

static void luid_str(char *out, const uint8_t *b) {
    sprintf(out, "%02x%02x%02x%02x-%02x%02x%02x%02x", b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]);
}

static void dxgi_adapters(void) {
    IDXGIFactory1 *f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) { printf("dxgi: CreateDXGIFactory1 failed\n"); return; }
    IDXGIAdapter1 *a;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
        printf("dxgi   adapter %u: \"%s\" pci=%04x:%04x rev=%02x luid=%08lx-%08lx vram=%llu MB%s\n", i, name,
               d.VendorId, d.DeviceId, d.Revision, (unsigned long)d.AdapterLuid.HighPart,
               (unsigned long)d.AdapterLuid.LowPart, (unsigned long long)(d.DedicatedVideoMemory >> 20),
               (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "");
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

static void vulkan_devices(void) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { printf("vulkan: vulkan-1.dll not loadable (%lu)\n", GetLastError()); return; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gpunames", 1, NULL, 0, (1u << 22) | (1u << 12)};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst = NULL;
    VkResult r = create(&ci, NULL, &inst);
    if (r) { printf("vulkan: vkCreateInstance -> %d\n", r); return; }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    uint32_t n = 0;
    enumerate(inst, &n, NULL);
    VkPhysicalDevice *devs = calloc(n ? n : 1, sizeof *devs);
    enumerate(inst, &n, devs);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &drv};
        VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        props2(devs[i], &p);
        uint32_t vendor, device, type;
        memcpy(&vendor, p.props + 8, 4); memcpy(&device, p.props + 12, 4); memcpy(&type, p.props + 16, 4);
        char luid[32] = "-";
        if (id.luidValid) luid_str(luid, id.deviceLUID);
        printf("vulkan device %u: \"%s\" pci=%04x:%04x type=%u driver=\"%s\" \"%s\" luid=%s\n", i,
               (const char *)p.props + 20, vendor, device, type, drv.driverName, drv.driverInfo, luid);
    }
}

int main(void) {
    const char *out = getenv("GPUNAMES_OUT");
    if (out && *out) freopen(out, "w", stdout);
    char *env[] = {"DXVK_FILTER_DEVICE_NAME", "VKD3D_FILTER_DEVICE_NAME", "MESA_VK_DEVICE_SELECT", "FLATPAK_ID",
                   "PRESSURE_VESSEL_RUNTIME", "container", NULL};
    for (int i = 0; env[i]; i++) printf("env    %s=%s\n", env[i], getenv(env[i]) ? getenv(env[i]) : "(unset)");
    dxgi_adapters();
    vulkan_devices();
    return 0;
}
