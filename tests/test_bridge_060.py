"""Bridge ABI 5 and the upstream 0.6.0 ordered-handoff shapes."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from dlssnr import assets, upstream

ROOT = Path(__file__).resolve().parents[1]
GCC = ['gcc', '-std=gnu11', '-O1', '-g', '-pthread', '-Werror=incompatible-pointer-types',
       '-fsanitize=address,undefined', '-fno-pie', '-no-pie']


class BridgeAbi5(unittest.TestCase):
    def test_abi4_package_rejected(self):
        manifest = json.loads((ROOT/'assets/manifest.json').read_text())
        manifest['files'] = {name: '0'*64 for name in upstream.COMPONENTS}
        assets.require_deployable(manifest)
        with self.assertRaises(RuntimeError):
            assets.require_deployable(dict(manifest, hip_bridge_abi=4))

    @unittest.skipUnless(shutil.which('gcc'), 'C compiler required')
    def test_pe_wrappers_forward_mapped_host_flag(self):
        src = (ROOT/'sources/trampoline/amdhip64_7_pe.c').read_text().replace(
            '#define EXPORT __declspec(dllexport)', '#define EXPORT __attribute__((ms_abi))')
        src = '#define __declspec(x)\n#define __stdcall __attribute__((ms_abi))\n' + src
        src += r'''
#include <assert.h>
static char host[4];
static int SYSV host_malloc(void **p, u64 size, unsigned flags) {
    assert(size == 4 && flags == 2); *p = host; return 0; }
static int SYSV host_devptr(void **d, void *h, unsigned flags) {
    assert(h == host && flags == 0); *d = (void *)0xd00d; return 0; }
int main(void) {
    DlssnrHipBridge t = {0}; void *p = 0, *d = 0; g = &t;
    t.p_hipHostMalloc = host_malloc; t.p_hipHostGetDevicePointer = host_devptr;
    t.magic = 0x344849504E524C44ULL;   /* ABI 4 table: refused */
    assert(hipHostMalloc(&p, 4, 2) == 3 && !p);
    t.magic = DLSSNR_HIP_MAGIC;
    assert(hipHostMalloc(&p, 4, 2) == 0 && p == host);
    assert(hipHostGetDevicePointer(&d, p, 0) == 0 && d == (void *)0xd00d);
    t.p_hipHostMalloc = 0; assert(hipHostMalloc(&p, 4, 2) == 3);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); (d/'test.c').write_text(src)
            # gc-sections drops the unused Windows imports so the PE source links on Linux.
            subprocess.run(GCC + ['-ffunction-sections', '-fdata-sections', '-I', str(ROOT/'sources/trampoline'),
                                  str(d/'test.c'), '-Wl,--gc-sections', '-o', str(d/'test')],
                           check=True, capture_output=True, timeout=30)
            subprocess.run([str(d/'test')], check=True, capture_output=True, timeout=15)

    @unittest.skipUnless(shutil.which('gcc'), 'C compiler required')
    def test_ordered_060_shapes(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d)/'ordered-060'
            subprocess.run(GCC + [str(ROOT/'tests/ordered_060_contract.c'), '-ldl', '-o', str(exe)],
                           check=True, capture_output=True, timeout=30)
            for scenario in range(4):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(exe), str(scenario)], check=True, capture_output=True, timeout=5,
                                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))


if __name__ == '__main__':
    unittest.main()
