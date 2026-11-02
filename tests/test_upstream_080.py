"""Upstream 0.8.0 (NeuralHub host) support: HIP functions resolved by name, bridge ABI 6."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from dlssnr import upstream

ROOT = Path(__file__).resolve().parents[1]


class Upstream080Hip(unittest.TestCase):
    def test_trampoline_exports_everything_080_resolves(self):
        src = (ROOT/'sources/trampoline/amdhip64_7_pe.c').read_text()
        exported = set(re.findall(r'EXPORT [\w\s*]+?\b((?:__)?hip\w+)\(', src))
        self.assertEqual(sorted(upstream.UPSTREAM_080_HIP_RESOLVED - exported), [])

    def test_bridge_table_covers_080(self):
        header = (ROOT/'native/hip_bridge.h').read_text()
        table = set(re.findall(r'\(\*p_((?:__)?hip\w+)\)', header))
        self.assertEqual(sorted(upstream.UPSTREAM_080_HIP_RESOLVED - table), [])
        self.assertEqual(header, (ROOT/'sources/trampoline/hip_bridge.h').read_text())

    @unittest.skipUnless(shutil.which('gcc'), 'C compiler required')
    def test_abi6_forwarding(self):
        src = r'''
#include "native/hip_bridge.c"
#include <assert.h>
static void *seen[8]; static u64 seen_n[4]; static int seen_i;
static int memset2d(void *d, u64 pitch, int v, u64 w, u64 h, void *st) {
    seen[0] = d; seen[1] = st; seen_n[0] = pitch; seen_n[1] = w; seen_n[2] = h; seen_i = v; return 0; }
static int tex(void **t, const void *r, const void *td, const void *v) {
    assert(r == (void *)0x10 && td == (void *)0x20 && !v); *t = (void *)0xbeef; return 0; }
static int d16(void *d, unsigned short v, u64 n, void *st) { seen[2] = d; seen_n[3] = n; seen_i = v; seen[3] = st; return 0; }
int main(void) {
    void *t = 0;
    g_hip_ok = 1;
    real.p_hipMemset2DAsync = memset2d; real.p_hipCreateTextureObject = tex; real.p_hipMemsetD16Async = d16;
    assert(g_bridge.magic == DLSSNR_HIP_MAGIC);
    assert(g_bridge.p_hipMemset2DAsync((void *)0x1000, 256, 7, 64, 3, (void *)0x2) == 0);
    assert(seen[0] == (void *)0x1000 && seen[1] == (void *)0x2 && seen_n[0] == 256 && seen_n[1] == 64 && seen_n[2] == 3 && seen_i == 7);
    assert(g_bridge.p_hipCreateTextureObject(&t, (void *)0x10, (void *)0x20, 0) == 0 && t == (void *)0xbeef);
    assert(g_bridge.p_hipMemsetD16Async((void *)0x3000, 0xabcd, 9, (void *)0x4) == 0);
    assert(seen[2] == (void *)0x3000 && seen_n[3] == 9 && seen_i == 0xabcd && seen[3] == (void *)0x4);
    real.p_hipStreamCreate = 0;
    assert(g_bridge.p_hipStreamCreate(&t) == 3);   /* missing in the runtime: refused, not crashed */
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); (d/'t.c').write_text(src)
            subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                            '-I', str(ROOT), str(d/'t.c'), '-ldl', '-o', str(d/'t')],
                           check=True, capture_output=True, timeout=60)
            subprocess.run([str(d/'t')], check=True, capture_output=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
