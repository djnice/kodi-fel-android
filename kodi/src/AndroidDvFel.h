/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/BitstreamConverter.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFrame;
struct felgpu_meta;

/*
 * Dolby Vision profile 7 FEL composition on Android with an Amlogic S5
 * (S928X), whose Dolby Vision hardware has no enhancement layer input; the
 * Android counterpart of CAMLDvFel (CoreELEC).
 *
 * MediaCodec decodes the base layer; the bitstream converter rewrites the RPU
 * to profile 8.1 with a no-op mapping and hands the original RPU and the EL
 * NAL units to this class (IDoviElSink). The dvfel kernel module, routed in
 * after the video composer (video_composer.0 -> dvfel -> video_render.0),
 * hands every decoded base layer frame to the GPU thread here, which composes
 * it with the enhancement layer and the original RPU mapping.
 *
 * EL decoding: a second MediaCodec instance (the Amlogic HEVC decoder) renders
 * into an AImageReader; every output buffer carries the decoder's 10-bit AFBC
 * frame, which dvfel decompresses into a linear buffer of the EL pool
 * (DVFEL_IOC_EL_IMPORT) for the GPU. Software decoding (FFmpeg) is the
 * fallback (/sdcard/dvfel_sw_el = 1, or the hardware decoder fails to start).
 *
 * Pairing: on Android the frame timestamp seen by the kernel is the video
 * composer's display time, not the packet pts. The kernel reports the
 * rpu_data_crc32 of the RPU the decoder got with the frame instead; each
 * packet's sent RPU CRC is filed with its pts, and a frame takes the earliest
 * pending pts with its CRC (frames arrive in display order).
 */
class CAndroidDvFel : public IDoviElSink
{
public:
  CAndroidDvFel() = default;
  ~CAndroidDvFel() override;

  // the dvfel kernel module is loaded and usable by this process
  static bool IsAvailable();

  // Dune HD firmware: installs the bundled kernel module (APK assets,
  // system/dvfel/dvfel.ko) with su into the firmware's boot hook directory
  // and loads it; in the background, the result goes to the log. dir: a
  // directory of the app that root can read (Context.getFilesDir()).
  static void InstallModuleAsync(const std::string& dir);
  // a Dolby Vision profile 7 stream with an EL plays without the module:
  // tells the user once and asks the root question again at the next start
  static void OnModuleMissing();

  // the RPU collected for the pending packet is from a FEL stream
  bool PendingIsFel() const;

  // Starts EL decoding and the GPU compositor; the kernel routes the video
  // composer through dvfel once the compositor registered. Must be done before
  // the first frame reaches the display.
  bool Start(int width, int height);
  void Stop();
  bool IsStarted() const { return m_started; }

  // seek / flush
  void Reset();

  // the data collected by the sink belongs to the packet with this pts (us)
  void CommitPacket(double pts);
  void DropPacket();

  // IDoviElSink
  void OnDoviRpu(const uint8_t* nal, uint32_t size) override;
  void OnDoviElNal(const uint8_t* nal, uint32_t size) override;
  void OnDoviRpuSent(const uint8_t* nal, uint32_t size) override;

private:
  struct ElPacket
  {
    int64_t pts;
    std::vector<uint8_t> data; // Annex-B
    std::shared_ptr<felgpu_meta> meta; // the RPU travels with the EL picture
    bool flush;
  };
  struct ElFrame
  {
    AVFrame* frame{nullptr}; // software decoded
    int buf{-1};             // hardware decoded: EL pool buffer
    std::shared_ptr<felgpu_meta> meta;
  };
  static constexpr int EL_POOL = 8;

  void ElThread();
  void HwElThread();
  void GpuThread(std::promise<bool> ready);
  // takes the EL frame of pts out of m_elFrames; release with ReleaseElFrame
  bool WaitForFrame(int64_t pts, ElFrame& el);
  void ReleaseElFrame(ElFrame& el); // m_lock held
  // the packet pts of a frame by the CRC of its RPU; -1 if not known
  int64_t PtsForRpuCrc(uint32_t crc);

  int m_elWidth{0};
  int m_elHeight{0};
  std::atomic<bool> m_hwEl{false};
  int m_elPoolFd[EL_POOL];
  bool m_elPoolBusy[EL_POOL]{};
  int m_width{0};
  int m_height{0};
  bool m_started{false};
  std::atomic<bool> m_stop{false};

  // per packet, filled by the sink (decode thread)
  std::vector<uint8_t> m_pendingRpu;
  std::vector<uint8_t> m_pendingEl;
  bool m_pendingCrcValid{false};
  uint32_t m_pendingCrc{0};
  std::shared_ptr<felgpu_meta> m_lastMeta;

  std::mutex m_lock;
  std::condition_variable m_elCond;    // EL input queue
  std::condition_variable m_readyCond; // new EL frame
  std::deque<ElPacket> m_elQueue;
  std::map<int64_t, ElFrame> m_elFrames; // by display pts
  std::map<int64_t, uint32_t> m_crcByPts; // RPU CRC of the packets given to the decoder
  int64_t m_lastJobPts{INT64_MIN}; // last frame the kernel asked for
  int m_noPairLogged{0};           // since start or the last seek
  int m_noCrcLogged{0};

  std::thread m_elThread;
  std::thread m_gpuThread;

  // statistics
  std::atomic<uint64_t> m_composed{0};
  std::atomic<uint64_t> m_passthrough{0};
  std::atomic<uint64_t> m_elDecoded{0};
  double m_gpuMsSum{0};
  std::vector<float> m_gpuMs; // per composed frame, GPU thread until Stop
};
