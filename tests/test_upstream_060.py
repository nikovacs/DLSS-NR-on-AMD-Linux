"""Upstream 0.6.0 pins, RDNA2 targets, per-family ROCm wheels and PCI-based GPU selection."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from dlssnr import cli, deploy, runtime, upstream

ROOT = Path(__file__).resolve().parents[1]


class Release060(unittest.TestCase):
    def test_current_version_consistent(self):
        manifest = json.loads((ROOT/'assets/manifest.json').read_text())
        self.assertEqual(upstream.VERSION, '0.6.0')
        self.assertEqual(manifest['mod_version'], '0.6.0')
        self.assertEqual(manifest['version'], '0.6.0')
        self.assertIn('/v0.6.0/', upstream.SETUP_URL)
        self.assertEqual(upstream.SETUP_BYTES, 59841536)
        self.assertEqual(upstream.PAYLOAD_BYTES, 56677888)
        self.assertEqual(manifest['external_files']['dlssnr_on_amd_setup.exe'], upstream.SETUP_SHA256)
        self.assertEqual(manifest['hip_bridge_abi'], 6)

    def test_sync_shaders_unchanged_only_relocated(self):
        # 0.6.0 carries the 0.5.0 shaders byte-for-byte, so vkd3d's flag hash is unchanged.
        self.assertEqual(upstream.FLAG_SHADER_SHA256,
                         '0b85aab8db0ee8ba14019e32e3839bb2d321cde974371b30eb3742955b53084b')
        self.assertEqual(upstream.FLAG_SHADER_HASH, '135ea1f88cbc832d')
        self.assertEqual(upstream.FLAG_SHADER_OFFSET, 546736)
        self.assertEqual({k: v[0] for k, v in upstream.GRAPHICS_WAIT_SHADERS.items()},
                         {'pixel': 548208, 'vertex': 549360})

    def test_new_hip_imports(self):
        self.assertLessEqual({'hipHostMalloc', 'hipHostGetDevicePointer'}, upstream.HIP_IMPORTS)

    @unittest.skipUnless(os.environ.get('DLSSNR_SETUP_060'), 'set DLSSNR_SETUP_060 to the official setup')
    def test_official_setup_inspects(self):
        payload, _, meta = upstream.inspect_setup(Path(os.environ['DLSSNR_SETUP_060']).read_bytes())
        self.assertEqual(len(payload), upstream.PAYLOAD_BYTES)
        self.assertEqual(meta['mod_version'], '0.6.0')

    def test_rdna2_discrete_targets(self):
        self.assertLessEqual({'gfx1030', 'gfx1031', 'gfx1032'}, cli.TARGETS)


class WheelFamily(unittest.TestCase):
    def test_family_mapping(self):
        cases = {100300: 'gfx103X-all', 100302: 'gfx103X-all', 110000: 'gfx110X-all',
                 110001: 'gfx110X-all', 120001: 'gfx120X-all', 90010: None, 101000: None}
        for version, family in cases.items():
            with self.subTest(version=version):
                self.assertEqual(runtime.wheel_family(version), family)

    def test_every_family_pinned_same_version(self):
        for family, digest in runtime.WHEELS.items():
            self.assertRegex(digest, r'^[0-9a-f]{64}$')
            url, sha = runtime.wheel_for(family)
            self.assertEqual(url, f'https://rocm.nightlies.amd.com/v2/{family}/'
                                  f'rocm_sdk_core-{runtime.WHEEL_VERSION}-py3-none-linux_x86_64.whl')
            self.assertEqual(sha, digest)
        self.assertEqual(runtime.wheel_for(None), (runtime.WHEEL_URL, runtime.WHEEL_SHA256))

    def test_detect_from_kfd_topology(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for node, version in (('0', 0), ('1', 100300)):   # CPU node reports 0
                (root/node).mkdir()
                (root/node/'properties').write_text(f'cpu_cores_count 8\ngfx_target_version {version}\n')
            self.assertEqual(runtime.detect_wheel_family(root), 'gfx103X-all')
            self.assertIsNone(runtime.detect_wheel_family(root/'missing'))


class GpuSelection(unittest.TestCase):
    def _pci(self, root, bus, cls, vendor='0x1002', device='0x73bf'):
        (root/bus).mkdir(parents=True)
        for name, value in (('class', cls), ('vendor', vendor), ('device', device)):
            (root/bus/name).write_text(value + '\n')

    def test_single_gpu_sets_no_filter(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._pci(root, '0000:2d:00.0', '0x030000')
            self._pci(root, '0000:2d:00.1', '0x040300')   # HDMI audio is not a display
            env = deploy._gpu_selection({'name': 'AMD Radeon RX 6800 XT', 'pci_bus_id': '0000:2d:00.0'}, root)
            self.assertEqual(env, {})

    def test_multi_gpu_selects_by_pci_id_not_name(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._pci(root, '0000:03:00.0', '0x030000', device='0x7550')
            self._pci(root, '0000:7c:00.0', '0x038000', device='0x15bf')
            env = deploy._gpu_selection({'name': 'AMD Radeon RX 9070 XT', 'pci_bus_id': '0000:03:00.0'}, root)
            self.assertEqual(env, {'MESA_VK_DEVICE_SELECT': '1002:7550'})
            with self.assertRaises(RuntimeError):
                deploy._gpu_selection({'name': 'x', 'pci_bus_id': '0000:99:00.0'}, root)

    def test_wrapper_never_filters_by_name(self):
        src = (ROOT/'dlssnr/deploy.py').read_text()
        self.assertNotIn("'DXVK_FILTER_DEVICE_NAME': gpu['name']", src)
        self.assertNotIn("'VKD3D_FILTER_DEVICE_NAME': gpu['name']", src)


class FlatpakSteam(unittest.TestCase):
    def test_game_inside_flatpak_home(self):
        with tempfile.TemporaryDirectory() as d:
            home = Path(d)/'.var/app/com.valvesoftware.Steam'
            exe = home/'.local/share/Steam/steamapps/common/Game/Game.exe'
            exe.parent.mkdir(parents=True); exe.write_bytes(b'MZ')
            self.assertEqual(deploy.flatpak_steam_home(exe, home), home)
            self.assertIsNone(deploy.flatpak_steam_home(Path(d)/'Games/Other.exe', home))

    def test_external_library_listed_by_flatpak_steam(self):
        with tempfile.TemporaryDirectory() as d:
            home = Path(d)/'.var/app/com.valvesoftware.Steam'
            steamapps = home/'.local/share/Steam/steamapps'
            library = Path(d)/'mnt/games/SteamLibrary'
            exe = library/'steamapps/common/Game/Game.exe'
            exe.parent.mkdir(parents=True); exe.write_bytes(b'MZ'); steamapps.mkdir(parents=True)
            (steamapps/'libraryfolders.vdf').write_text(
                '"libraryfolders"\n{\n "0"\n {\n  "path" "%s"\n }\n "1"\n {\n  "path" "%s"\n }\n}\n'
                % (home/'.local/share/Steam', library))
            self.assertEqual(deploy.flatpak_steam_home(exe, home), home)

    def test_runner_list_matches_the_games_steam(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            home = d/'.var/app/com.valvesoftware.Steam'
            fp_steam = home/'.local/share/Steam'
            shared = d/'mnt/SteamLibrary'
            fp_game = fp_steam/'steamapps/common/Game/Game.exe'
            native_game = d/'.local/share/Steam/steamapps/common/Other/Other.exe'
            shared_game = shared/'steamapps/common/Shared/Shared.exe'
            for exe in (fp_game, native_game, shared_game):
                exe.parent.mkdir(parents=True); exe.write_bytes(b'MZ')
            (fp_steam/'steamapps/libraryfolders.vdf').write_text(
                '"libraryfolders"\n{\n "0"\n {\n  "path" "%s"\n }\n "1"\n {\n  "path" "%s"\n }\n}\n'
                % (fp_steam, shared))
            fp_proton = fp_steam/'steamapps/common/Proton - Experimental'
            native_proton = d/'.local/share/Steam/steamapps/common/Proton - Experimental'
            shared_proton = shared/'steamapps/common/Proton 10.0'
            system_proton = d/'usr/share/steam/compatibilitytools.d/proton-system'
            for p in (fp_proton, native_proton, shared_proton, system_proton):
                p.mkdir(parents=True)
            ok = lambda runner, exe: deploy.runner_belongs_to_game_steam(runner, exe, home)
            # Flatpak game: its own Proton and shared libraries only (the reported bug: native listed too).
            self.assertTrue(ok(fp_proton, fp_game))
            self.assertTrue(ok(shared_proton, fp_game))
            self.assertFalse(ok(native_proton, fp_game))
            self.assertFalse(ok(system_proton, fp_game))
            # Native game: anything outside Flatpak Steam's private home.
            self.assertTrue(ok(native_proton, native_game))
            self.assertTrue(ok(system_proton, native_game))
            self.assertFalse(ok(fp_proton, native_game))
            # A game in a library Flatpak Steam lists is treated as Flatpak's.
            self.assertTrue(ok(shared_proton, shared_game))
            self.assertFalse(ok(native_proton, shared_game))

    def test_no_flatpak_install(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(deploy.flatpak_steam_home(Path(d)/'x.exe', Path(d)/'missing'))


if __name__ == '__main__':
    unittest.main()
