"""Version and staged-payload contract, not a GPU performance test."""
from pathlib import Path
import hashlib
import json
import unittest
from dlssnr import upstream, deploy

ROOT=Path(__file__).resolve().parents[1]

class Release050Behaviour(unittest.TestCase):
    """0.5.0 behaviour that later releases keep."""
    def test_preserve_settings_on_update(self):
        config=b'[DlssNrOnAmd]\nQuality=reference\nOverlayKey=F8\nScale=0.04\nLocalStructure=0.3\n'
        out=deploy._ini(config,0,update=True)
        for line in config.splitlines()[1:]:self.assertIn(line+b'\n',out)
        self.assertEqual(deploy._ini(out,0,update=True),out)

    def test_amd_handheld_support_not_silently_enabled(self):
        from dlssnr import cli
        # Steam Deck (gfx1033), Z1/780M-class (gfx1103) and Strix (gfx115x) APUs stay off.
        for apu in ('gfx1033','gfx1035','gfx1036','gfx1103','gfx1150','gfx1151'):
            self.assertNotIn(apu,cli.TARGETS)

if __name__=='__main__':unittest.main()
