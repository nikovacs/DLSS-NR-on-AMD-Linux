# Release notes

## Unreleased — upstream 0.6.0, RDNA2, Flatpak Steam

- Stage upstream 0.6.0. Its synchronization shaders are byte-identical to 0.5.0
  at new offsets, so the vkd3d patch and flag hash are unchanged.
- Bridge ABI 5: forward `hipHostMalloc` / `hipHostGetDevicePointer` (0.6.0 maps a
  host status flag at network setup).
- Ordered handoff: accept 0.6.0's `k_expo_resolve` (FreshExposure) as the first
  work-queue kernel, and treat a repeated frame number on an idle resource as a
  producer counter restart (0.6.0 reuses pooled flag buffers after the game
  re-creates its upscaler).
- Load ROCm on a plain thread: on a Wine thread inside the Flatpak Steam
  container the loader hung or rejected valid ELF files.
- Preload `libamd_comgr.so.3` beside the selected HIP library: distribution HIP
  loads it by name at device initialization, which fails inside the container.
- Select the game's GPU by PCI vendor:device (`MESA_VK_DEVICE_SELECT`) on
  multi-GPU systems and not at all on single-GPU systems. The previous
  `DXVK/VKD3D_FILTER_DEVICE_NAME` used the HIP name, which differs from the name
  Proton reports (Wine uses the PCI ID database, e.g. "Radeon RX 6800/6800 XT /
  6900 XT"), so DXVK found no adapter and games failed to start DirectX 12.
- Allow `gfx1030`/`gfx1031`/`gfx1032` and choose the AMD core wheel per GPU family
  (`gfx103X`, `gfx110X`, `gfx120X`, same pinned ROCm version) from the kernel's
  KFD topology.
- Flatpak Steam: keep runtime and bridge inside Flatpak Steam's home.
- Runner list: offer only the Proton/Wine runners of the Steam installation that
  owns the game (native and Flatpak Steam keep separate runners).
- Several runtime copies in one process (AMDNR "Neural passes" loads
  `dlssnr_amd_pass1..3.dll`): kernel roles are tracked per copy. A second copy
  overwrote the first copy's `k_flag_wait`, which then ran unmanaged until the
  2 s timeout and removed the device.
- Queue affinity hands a resource over to another queue once no in-flight job
  pins it (bounded wait while one does) instead of refusing the claim and
  removing the device. Refused claims now log their reason.
- Trampoline: `hipInit`, and the adapter LUID in `hipGetDevicePropertiesR0600`
  when Linux HIP reports none and Wine has exactly one adapter (hosts such as
  AMDNR pair HIP with D3D12 by LUID).
- Checked with AMDNR 0.3.5.8R11 hosting upstream 0.6.0 (Hogwarts Legacy, RX 6800
  XT): neural rendering runs; live NR-resolution, interleave and Neural-passes
  changes work (15,209 frames, no device removal).
- Checked: Hogwarts Legacy (FSR 3) on RX 6800 XT / gfx1030, Flatpak Steam,
  Proton Experimental, Fast arithmetic: neural rendering runs, about 121 ms of
  network time per frame at 3414x961 render resolution (5120x1440, FSR Quality).
  RDNA2 has no matrix instructions; expect low frame rates. Other games, RDNA3/4
  regression and gfx1031/gfx1032 are not yet re-checked with this build.

## 0.5.0 — Linux / Proton

- Stage upstream 0.5.0 using pinned bytes, checksum and embedded payload bounds.
- Preserve the existing Linux bridge and optional-backend policy.
- Accept Python 3.10 for the original installer path.
- Clarify the hidden per-executable mod folder and install-versus-launch sequence.
- Report the failing stage and never offer a launch command for an invalid install.
- Check venv prerequisites before the optional HIP wheel download.
- Validate startup, gameplay and Quality/Balanced switching in Cyberpunk 2077,
  007 First Light and Atomic Heart on RX 9070 XT, with Fast arithmetic.
- Record Cyberpunk single-run benchmark changes (+1.27% Quality, +1.54% Balanced)
  and separately labeled gameplay minimum/maximum changes for the other games.
- Keep intermittent processing spikes and untested conditions explicit.


## 0.4.3

Update to the original upstream 0.4.3 runtime, retaining the Linux priority-stream
bridge and full guided installer. Includes Fast/Reference settings, configurable
overlay key and the upstream overlay/settings fixes. Updates preserve existing
models, visual choices and original-file backups.

Optional lmxxf optimizations are selected per release when they provide a verified benefit. Version 0.4.3 uses the original upstream inference backend.

Current gameplay checks cover Cyberpunk 2077, 007 First Light and Atomic Heart
on RX 9070 XT in Fast mode. Cyberpunk benchmark averages: 55.22 FPS Quality and
64.87 FPS Balanced, with frame generation enabled; see the full notes for settings.
Intermittent processing spikes remain unresolved. No Linux FPS percentage is claimed.

[Full 0.4.3 notes](docs/releases/0.4.3.md) · [Installation](docs/INSTALL.md)

## Previous releases

The following entry describes the previously published release, not 0.4.3.

## 0.3.1-lmxxf

### Changes

- Add the lmxxf HIP backend while retaining the complete neural network.
- Optimize attention, remove redundant conversions and reuse GPU resources across frames.
- Reduce CPU allocation and logging overhead; record slow jobs automatically.
- Guide installation and updates through launcher, game, Proton and upscaler selection.
- Prepare model data automatically, reuse it across games and preserve launch options.

### Performance

| Game | Previous Linux release | 0.3.1-lmxxf | Average FPS change |
| --- | ---: | ---: | ---: |
| Cyberpunk 2077 | 41.16 FPS | 46.61 FPS | +13.2% |

Built-in benchmark versus `v0.3.1-linux.1`, with Balanced FSR, RT Ultra and
frame generation enabled in both runs. This is a single comparison, not a
guarantee for other games or configurations.

### Compatibility

This release has been checked in Cyberpunk 2077. The 007 First Light
and Atomic Heart setup profiles are retained from earlier validation; no new
performance result is claimed for them.

The optimized backend currently targets `gfx1201`. Other GPUs supported by the
installer retain the original backend. Python 3.11+, gcc/g++ and HIP 7 are required.
NVIDIA model data is not included. This remains experimental software.

### Install or update

Close the game, extract the portable package and run `./install.sh`. Follow the
guided steps and copy the final launch instructions. Existing model data,
visual settings and original-file backups are retained.

[Installation guide](docs/INSTALL.md) · [Earlier releases](https://github.com/bulacha3/DLSS-NR-on-AMD-Linux/releases)
