# Kodi FEL for Android (Dune HD / R_volution, Amlogic S928X)

> **Proof of concept.** Experimental build for enthusiasts. It loads a kernel module on your player –
> use at your own risk.

Dolby Vision **Profile 7 FEL** (full enhancement layer) playback at full quality on the Android firmware of
Amlogic S928X players (Dune HD, R_volution), in a modified Kodi 22. The S5 Dolby Vision hardware has no
enhancement layer input, so the two layers are composed on the GPU:

```
Kodi demux ─ BitstreamConverter: RPU → profile 8.1 (no-op mapping), EL NAL units → CAndroidDvFel
  ├─ BL: MediaCodec (Dolby Vision decoder) → HWC → video_composer.0
  │        → dvfel (kernel): VICP A, AFBC → linear 10-bit 4:4:4 ─┐
  └─ EL: 2nd MediaCodec (HEVC) → AImageReader → dmabuf (uvm vframe, 10-bit AFBC)
           → DVFEL_IOC_EL_IMPORT: VICP → linear 4:4:4 ─────────────┤
                                     GPU (felgpu): Lanczos-3 EL upscale, polynomial/MMR prediction,
                                     NLQ, 12-bit composition, dither ←──┘ (+ original RPU)
  → GPU writes the dvfel slot's display buffer (linear 10-bit 4:4:4)
  → video_render.0 → amvideo (VD1 reads it directly) → Dolby Vision core (profile 8.1)
```

Pairing BL and EL frames: the kernel reports the `rpu_data_crc32` of each displayed frame's RPU; the
EL pictures are paired in display order (as on CoreELEC). Hardware EL output is bit-identical to the
FFmpeg software EL path; GPU composition takes ~31–34 ms per 4K frame, so 4K 24p plays in real time.

## Download

Releases: <https://github.com/djnice/kodi-fel-android/releases> (APK, read `README.txt`).

## Requirements

- Amlogic S928X Dune HD or R_volution player with the Dune HD **R22 or R24** Android firmware.
  Tested: Dune HD Pro One 8K Plus, firmware `260303_1721_r24`; Dune HD Pro 8K Plus, firmware
  `250815_1012_r22`. One module build for all of them (no symbol version differences between these two);
  on other builds / models please report how it works (kodi.log `CAndroidDvFel` lines – they also list
  kernel symbols whose versions differ from the tested firmware).
- Root access for the one-time module installation (the firmware's `sud`; Kodi FEL asks to switch it on
  for the installation and switches it off again).

## How the module gets installed

Kodi FEL ("Kodi FEL", `org.xbmc.kodi.fel`, installs next to the official Kodi) carries `dvfel.ko` in
its assets (`system/dvfel/`). On start, if the module is not loaded (or older):

1. `su` – if the firmware's root access is off (`persist.vendor.root_access=0`, `sud` stopped), a Yes/No
   dialog asks to switch it on for the installation; the previous value is restored afterwards.
2. The module is loaded. The firmware kernel only **warns** about symbol CRC mismatches
   (`disagrees about version of symbol`) and loads it anyway – one build serves all firmware versions;
   such warnings are written to kodi.log (`tools/fwcheck.sh` lists them for a player over adb).
3. The module goes to `<FS_PREFIX>/config/dvfel/dvfel.ko` and a boot script to
   `<FS_PREFIX>/config/boot/dvfel.sh`, which the firmware runs as root at every boot (R24: `binit.sh`,
   R22: the `dunehd_init` service script `/system/xbin/dunehd/init` itself).

## Repository

| Path | Content |
|---|---|
| `kernel/` | `dvfel` kernel module (GPL-2.0+): vfm node between video_composer and video_render, VICP A, EL import, GPU job interface with direct display buffers (`dvfel_uapi.h`, `DVFEL_IOC_REG_BUFS2`; the older AFBC output path with VICP B is still there for older compositors). Builds for the Dune 5.4 GKI kernel. |
| `kodi/patches/` | Patches for upstream Kodi `22.0rc1-Piers` (`28ea2eac1e`), in this order: 9900 FEL composition + module installer, 9901 vsync period from Choreographer + startup crash fixes, 9902 video reference clock at the display's actual refresh rate. |
| `kodi/src/` | The main new sources from 9900 for reading: `AndroidDvFel.*`, `felgpu.*` (GPU composer). |
| `tools/` | `kbuild.sh` (module build with the firmware's CRCs), `modver.py` (CRC database/symvers from firmware modules), `fwcheck.sh` (does a build fit a player? trial load over adb), `elprobe.cpp` (EL import probe), `sync.sh` (build script used for the releases). |
| `docs/NOTES.md` | Technical notes and pitfalls of this firmware. |

## Building (outline)

- Kodi: upstream `22.0rc1-Piers` + `kodi/patches/*`, Android armeabi-v7a, NDK r28c, API 24, depends with
  `--disable-debug`; cmake `-DAPP_PACKAGE=org.xbmc.kodi.fel`. The module build goes to
  `system/dvfel/dvfel.ko`.
- Module: clang 11.0.1 (`LLVM=1`, as the firmware kernel), a kernel tree configured like the firmware, the
  Amlogic media headers matching the firmware's structures, and the firmware's symbol CRCs
  (`modver.py db/symvers` from its vendor modules and `aml_media.ko`) – see `tools/kbuild.sh`.

## Status / known limits

- Proof of concept: two firmware versions / two models tested. 4K 50/60p FEL is too heavy (GPU time).
- The Dune auto frame rate does not switch back after stop until you return to the Dune home screen.
- After a seek, 1–2 frames may be shown with the RPU mapping only (no EL residual).
- Kodi still drops a frame now and then (0–3 a minute measured), like without FEL: its clock and the
  display drift apart.

## Changes

- **v0.1.3-poc** (module unchanged, `2026.10.06.2`): R22 firmware support – the installer also finds the
  boot hook of R22 (`/system/xbin/dunehd/init`; v0.1.2 reported "no Dune HD firmware boot hook" there).
  Tested on a Dune HD Pro 8K Plus with `250815_1012_r22`.
- **v0.1.2-poc** (module `2026.10.06.2`):
  - EL decoder no longer loses pictures (each EL access unit now ends with its RPU as in a demuxed EL
    stream – before, the decoder timed out on some pictures, mostly at movie starts).
  - Frames are no longer shown without EL every few seconds (RPU CRC pairing fix).
  - No more freezes after fast seeks / chapter jumps and no more memory leak (video_composer repeat
    frames).
  - Direct display: the GPU writes the displayed frame, no AFBC re-compression (VICP B gone);
    frames are taken from video_composer at the vsync (no double takes).
  - "Sync playback to display" works (was ~260 dropped frames a minute: wrong refresh rate in Kodi's clock).
  - The installer never replaces a newer kernel module with an older one; flickering after a seek and
    1–2 fps after a direct file switch fixed.
- **v0.1.1-poc**: one kernel module for all R24 firmware.
- **v0.1.0-poc**: first release.

## License

GPL-2.0-or-later (Kodi and the kernel module). Dolby Vision is a trademark of Dolby Laboratories; this
project is not affiliated with Dolby, Dune HD, R_volution or the Kodi Foundation.
