"""Optional per-kernel profiler (DLSSNR_PROFILE=1) of the bridge."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Profiler(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gcc'), 'C compiler required')
    def test_sampling_and_aggregation(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d)/'prof'
            subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                            str(ROOT/'tests/nr_profile_contract.c'), '-ldl', '-o', str(exe)],
                           check=True, capture_output=True, timeout=60)
            subprocess.run([str(exe)], check=True, capture_output=True, timeout=30,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))

    def test_off_by_default(self):
        src = (ROOT/'native/nr_profile.c').read_text()
        self.assertIn('getenv("DLSSNR_PROFILE")', src)
        self.assertIn('!strcmp(v, "1")', src)


if __name__ == '__main__':
    unittest.main()
