# Technical notes (Dune HD R24 Android firmware, Amlogic S928X)

Findings from bringing up FEL composition on the Dune HD Pro One 8K Plus (firmware `260303_1721_r24`,
kernel `5.4.210` GKI, Android 11 userdebug, SELinux permissive).

## Video path

- MediaCodec (OMX, V4L2 decoder) → HWC → `video_composer.0` → `video_render.0` → amvideo. The vfm map
  `vcom-map-0` is created by video_composer at playback start. dvfel takes over the unused PIP map slot
  `dvelpath2` (`video_composer.0 dvfel video_render.0`), which precedes `vcom-map-0` (vfm uses the first
  valid map naming the provider).
- video_composer hands frames out at display time and allows at most 2 outstanding: dvfel releases the
  original right after VICP A ("early release", copying the Dolby Vision metadata into its slot).
  The release fence only signals for `vf->rendered` frames – the copy's flag must be carried over.
- **Repeated buffers**: when HWC sends the same buffer again (pause, seek, chapter jump), video_composer
  only increments `repeat_count` of the frame its receiver still holds and releases the repeats with it.
  A frame given back too early leaks the repeats: the decoder waits for their release fences
  (`OMX_AmlogicPeerBufferWrapper: wait fence 1001ms` ×3 → codec error, playback frozen) and codec_mm
  leaks until reboot. dvfel keeps the newest upstream frame until the next one arrives (`up_held`).
  Leak check: `/sys/class/video_composer/total_get_count - total_put_count`.
- **Pacing**: video_composer releases frames relative to the *last vsync* (it expects to be asked from the
  display's vsync interrupt). Polling it in between takes frames early and two within one vsync
  (`vpp_drop_cnt` in `/sys/class/video_composer/drop_cnt`). dvfel takes at most one per vsync
  (`get_vsync_count()`, `vc_pace`), two only with a backlog.
- Frame identification: on Android `pts_us64` is the display time, not the packet pts. dvfel reports the
  `rpu_data_crc32` of the frame's RPU (`DVFEL_IOC_WAIT_JOB2`), Kodi pairs it with the packet. The decoder
  hands the RPU to the kernel already unescaped and dvfel removes `00 00 03` once more: Kodi computes the
  CRC the same way, or CRCs starting with `03` after two zero bytes never match.
- **Direct display** (module 2026.10.06.2, `DVFEL_IOC_REG_BUFS2`): the VD1 reads linear 10-bit 4:4:4
  (`VIDTYPE_VIU_444`, `bit_mode 2`) – the compositor's frames are shown as they are, one compositor
  buffer per slot, no VICP B. Verified with the VD1 probe (`/sys/class/video/matrix_*`): luma identical
  to the AFBC path, chroma ±1–3 (4:4:4 instead of 4:2:0).

## VICP (5.4 GKI driver)

- The AFBC encoder is never switched off: every VICP operation needs a valid AFBC sink, or the next
  operation writes to stale addresses (ISR timeouts until reboot) – `a_fbc_sink`.
- **The first VICP operation after boot latches the HDR state**: if its input vframe carries a PQ
  `signal_type`, every later output gets a luma curve until reboot (chroma intact). The private input copy
  has `signal_type` cleared (`vin_sdr`).
- `vicp_process_enable(1)` (RDMA) caused silent resets; reading VICP registers outside an operation hangs
  the bus (watchdog reboot).
- Times: A (4K AFBC → linear) ~13 ms, B (linear → AFBC, plain mode) 13–22 ms (no longer used with
  direct display), EL import (1080p) ~3.5–4 ms.

## Hardware EL

- The HEVC decoder's CPU outputs are 8-bit only (V4L2 capture NV12/NV21/YUV420; no P010).
- Rendered into an `AImageReader` (PRIVATE, GPU_SAMPLED_IMAGE, max 2 images – the decoder allows 12
  output buffers in total), every image's dmabuf is a uvm buffer carrying the decoder vframe: 10-bit AFBC
  (`type 0x509000`, `bitdepth 0x2a00`). `dmabuf_get_vframe()` + VICP give a bit-exact linear picture.
- **Each EL access unit must end with the RPU NAL unit** (type 62), as in a demuxed EL stream: without a
  NAL unit after the last slice, the HEVC decoder's back end times out on some pictures (200 ms,
  `timeout_process_back`), and the pictures referencing them are lost until the next IRAP.

## GPU

- Mali-G57 under `simple_ondemand` stays at 666 MHz for the composer (37 ms per 4K frame); dvfel raises the
  devfreq `min_freq` to the maximum (830 MHz, 31 ms) while a compositor is registered (`gpu_boost`).
- A high priority EGL context made it worse (Kodi's own GPU work waits).

## Kodi on this firmware

- The Dune auto frame rate switches the HDMI mode behind Android's back (Android reports one mode only).
  Kodi's `GetNextFrameTime` used its own requested 59.94 Hz → target times between vsyncs at 24 Hz → two
  buffers per vsync → SurfaceFlinger drops (video_composer `player_drop_cnt`). Fix: vsync period measured
  from the Choreographer callbacks (patch 9901). Note: debug logging hides the effect.
- The same stale 59.94 Hz broke the video reference clock ("Sync playback to display"):
  `CVideoSyncAndroid` counted 2 and 3 vblanks alternately per 23.976 Hz callback (~260 dropped frames a
  minute). Fix: the display's actual mode (`Display.getMode()`, the Choreographer rate decides if they
  disagree), clock restart when the rate changes (patch 9902).
- The AFR restores the GUI mode only when returning to the Dune shell, not on playback stop.
- Starting Kodi while the activity goes to the background (sleeping box, launcher in front): `run()` gives
  up waiting for the window and the next `onStart` created a second `std::thread` (std::terminate);
  `onNewIntent` before Kodi ran logged through a null logger (SIGSEGV). Both fixed in 9901.

## Root and module loading

- `su` (Koushik Superuser client) talks to the `sud` daemon, which runs while `persist.vendor.root_access`
  is empty (default) or `1`. On this permissive firmware an app may set that property itself.
- Module symbol CRC mismatches only produce a warning (`disagrees about version of symbol`) – the module
  loads anyway, so one build serves all firmware versions. A mismatch means a type in that symbol's
  signature changed (e.g. `struct vframe_s`): the first suspect if a firmware misbehaves. The installer
  logs them; `tools/fwcheck.sh` lists them for a player.
- `<FS_PREFIX>` comes from `FSP=` in `/system/xbin/dunehd/init` (`/data/data/com.dunehd.app` on Dune);
  `/system/dunehd/firmware/scripts/binit.sh` runs `<FS_PREFIX>/config/boot/*` as root at boot.
