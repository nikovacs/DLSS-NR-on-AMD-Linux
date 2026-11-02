"""Upstream 0.8.0 (NeuralHub host) support: HIP functions resolved by name, bridge ABI 6."""
import os
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


VKD3D_SOURCE = ROOT/'build/vkd3d-source/libs/vkd3d'


class Vkd3dOrderedContract(unittest.TestCase):
    """CPU contract for the patched nr_ordered_commands.h (needs the patched source tree:
    python3 sources/fetch_vkd3d.py build/vkd3d-source)."""
    @unittest.skipUnless(shutil.which('gcc') and (VKD3D_SOURCE/'nr_ordered_commands.h').is_file(),
                         'patched vkd3d source tree required')
    def test_contract(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d)/'vkc'
            subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', '-I', str(VKD3D_SOURCE),
                            str(ROOT/'tests/vkd3d_ordered_contract.c'), '-o', str(exe)],
                           check=True, capture_output=True, timeout=60)
            subprocess.run([str(exe)], check=True, capture_output=True, timeout=30,
                           env={'ASAN_OPTIONS': 'detect_leaks=0', 'PATH': '/usr/bin:/bin'})

    def test_patch_matches_source_tree(self):
        if not (VKD3D_SOURCE/'nr_ordered_commands.h').is_file():
            self.skipTest('patched vkd3d source tree required')
        patch = (ROOT/'sources/vkd3d-proton-ordered.patch').read_text()
        header = (VKD3D_SOURCE/'nr_ordered_commands.h').read_text()
        added = [line[1:] for line in patch.split('diff --git a/libs/vkd3d/nr_ordered_commands.h')[1].split('diff --git')[0].splitlines()
                 if line.startswith('+') and not line.startswith('+++')]
        self.assertEqual('\n'.join(added).strip(), header.strip())


class LoaderLockGuard(unittest.TestCase):
    """The trampoline's in-memory guard for the 0.8.0 host must match that exact build."""
    def _sites(self):
        src = (ROOT/'sources/trampoline/amdhip64_7_pe.c').read_text()
        sites = re.findall(r'\{0x([0-9a-f]+), 0x([0-9a-f]+), (\d+), \{([0-9a-fx,]+)\}, 0, 0\}', src)
        self.assertEqual(len(sites), 2)
        size = int(re.search(r'#define NH080_SIZE_OF_IMAGE (0x[0-9a-f]+)u', src).group(1), 16)
        return [(int(r, 16), int(slot, 16), int(n), bytes(int(b, 16) for b in bs.split(','))) for r, slot, n, bs in sites], size

    def test_sites_are_whole_position_independent_prologues(self):
        sites, _ = self._sites()
        for rva, slot, n, prologue in sites:
            self.assertEqual(len(prologue), n)
            self.assertGreaterEqual(n, 14)                       # room for the 14-byte absolute jump
            self.assertEqual(prologue[:1], b'\x55')              # push rbp: a function entry
            self.assertNotIn(b'\x8d\x05', prologue)             # no RIP-relative operand moved

    @unittest.skipUnless(os.environ.get('DLSSNR_HOST_080'), 'set DLSSNR_HOST_080 to neuralhub_host.dll 0.8.0')
    def test_sites_match_the_pinned_host(self):
        import struct
        data = Path(os.environ['DLSSNR_HOST_080']).read_bytes()
        sites, size = self._sites()
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        self.assertEqual(struct.unpack_from('<I', data, pe + 24 + 56)[0], size)
        nsec = struct.unpack_from('<H', data, pe + 6)[0]; opt = struct.unpack_from('<H', data, pe + 20)[0]
        def off(rva):
            for i in range(nsec):
                s = pe + 24 + opt + 40 * i
                vsz, va, rsz, rp = struct.unpack_from('<IIII', data, s + 8)
                if va <= rva < va + max(vsz, rsz): return rva - va + rp
        for rva, slot, n, prologue in sites:
            self.assertEqual(data[off(rva):off(rva) + n], prologue)
            # The detour calls the original through the saved slot right after its prologue.
            body = data[off(rva):off(rva) + 0x40]
            disp_at = body.index(b'\xff\x15') + 2
            disp = struct.unpack_from('<i', body, disp_at)[0]
            self.assertEqual(rva + disp_at + 4 + disp, slot)


if __name__ == '__main__':
    unittest.main()
