"""PE trampoline additions for hosts that probe HIP themselves (AMDNR): hipInit and the adapter LUID."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = ['gcc', '-std=gnu11', '-O1', '-g', '-Werror=incompatible-pointer-types',
       '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections']

HARNESS = r'''
#include <assert.h>
#include <string.h>
static int adapters = 1, devices = 1, enum_calls;
static int SYSV fake_count(int *n) { *n = devices; return 0; }
static unsigned char linux_luid[8];
static int SYSV fake_props(void *p, int id) {
    (void)id; memset(p, 0, 1024); strcpy((char *)p, "AMD Radeon RX 6800 XT");
    memcpy((unsigned char *)p + 272, linux_luid, 8); return 0; }
static long __stdcall fake_enum2(KmtEnumAdapters2 *q) {
    ++enum_calls;
    if (q->count < (unsigned)adapters) return 1;
    for (int i = 0; i < adapters; ++i) {
        q->adapters[i] = (KmtAdapterInfo){0x40000000u + i, 0x3f2u + i, 0, 1, 0};
    }
    q->count = adapters; return 0; }
HANDLE __stdcall LoadLibraryA(const char *name) { return strcmp(name, "gdi32.dll") ? 0 : (HANDLE)1; }
void *__stdcall GetProcAddress(HANDLE m, const char *name) {
    return m == (HANDLE)1 && !strcmp(name, "D3DKMTEnumAdapters2") ? (void *)fake_enum2 : 0; }

int main(void) {
    DlssnrHipBridge t = {0}; g = &t;
    static unsigned char p[8192];
    t.magic = DLSSNR_HIP_MAGIC; t.p_hipGetDeviceCount = fake_count; t.p_hipGetDevicePropertiesR0600 = fake_props;
    /* hipInit: only flags 0, maps "no device" like HIP. */
    assert(hipInit(1) == 1);
    assert(hipInit(0) == 0);
    devices = 0; assert(hipInit(0) == 100); devices = 1;
    /* One adapter: Linux HIP's zero LUID becomes Wine's adapter LUID. */
    assert(hipGetDevicePropertiesR0600(p, 0) == 0);
    unsigned lo, hi, mask; memcpy(&lo, p + 272, 4); memcpy(&hi, p + 276, 4); memcpy(&mask, p + 280, 4);
    assert(lo == 0x3f2 && hi == 0 && mask == 1);
    /* Several adapters: no guess, the LUID stays zero. */
    adapters = 2; assert(hipGetDevicePropertiesR0600(p, 0) == 0);
    memcpy(&lo, p + 272, 4); assert(lo == 0);
    /* A runtime that already reports a LUID is never overridden (and D3DKMT is not consulted). */
    adapters = 1; linux_luid[0] = 0x77; enum_calls = 0;
    assert(hipGetDevicePropertiesR0600(p, 0) == 0 && p[272] == 0x77 && !enum_calls);
    /* No bridge: refused as before. */
    t.magic = 0; assert(hipInit(0) == HIP_ERROR_NOT_INITIALIZED);
    return 0;
}
'''


class AmdnrHostProbe(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gcc'), 'C compiler required')
    def test_hipinit_and_single_adapter_luid(self):
        src = (ROOT/'sources/trampoline/amdhip64_7_pe.c').read_text().replace(
            '#define EXPORT __declspec(dllexport)', '#define EXPORT __attribute__((ms_abi))')
        src = '#define __declspec(x)\n#define __stdcall __attribute__((ms_abi))\n' + src + HARNESS
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); (d/'test.c').write_text(src)
            subprocess.run(GCC + ['-I', str(ROOT/'sources/trampoline'), str(d/'test.c'),
                                  '-Wl,--gc-sections', '-o', str(d/'test')],
                           check=True, capture_output=True, timeout=30)
            subprocess.run([str(d/'test')], check=True, capture_output=True, timeout=15)

    def test_kernel32_imports_declared(self):
        names = (ROOT/'sources/trampoline/kernel32.def').read_text().split()
        self.assertIn('LoadLibraryA', names)
        self.assertIn('GetProcAddress', names)


if __name__ == '__main__':
    unittest.main()
