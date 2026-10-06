/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AndroidDvFel.h"

#include "felgpu.h"
#include "dvfel_uapi.h"
#include "ServiceBroker.h"
#include "application/Application.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "dialogs/GUIDialogYesNo.h"
#include "filesystem/File.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <libdovi/rpu_parser.h>

#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241 /* 'AB24' */
#endif

namespace
{
constexpr const char* DVFEL_DEV = "/dev/dvfel";
constexpr int GPU_BUFFERS = 3;
constexpr int EL_TAPS = 6;             // Lanczos-3, see felgpu.h
constexpr int EL_WAIT_MS = 80;         // wait for the EL frame of a job (the kernel waits 150)
// packets are fed well ahead of display; EL decoding is paced to stay at most
// EL_LEAD_US ahead of the frame the kernel asked for last
constexpr int64_t EL_LEAD_US = 350000;
constexpr size_t MAX_EL_FRAMES = 24;   // decoded EL frames kept (6 MB each at 4K)
constexpr size_t MAX_EL_QUEUE = 400;   // compressed EL packets
constexpr size_t MAX_CRC = 2048;       // packets waiting for their frame
constexpr size_t MAX_META = 512;       // RPU mappings kept for frames without EL
constexpr size_t CRC_SEARCH = 64;      // pending packets a frame may skip (dropped frames)
constexpr int64_t PTS_TOLERANCE_US = 1000;

// hardware EL decoder: renders into an AImageReader (at most 2 images, the
// decoder allows 12 output buffers in all); the dma-buf of each image
// carries the decoder frame
constexpr const char* HW_EL_CODEC = "OMX.amlogic.hevc.decoder.awesome2";
constexpr int HW_EL_IMAGES = 2;
constexpr int HW_EL_IMAGE_WAIT_MS = 40;
// pictures counted as still in the decoder for the pool bound (pictures the
// decoder drops as broken never come out)
constexpr int64_t EL_IN_DECODER_MAX = 6;

// files for tests, readable by the app (external storage)
constexpr const char* SW_EL_FILE = "/sdcard/dvfel_sw_el";
constexpr const char* DITHER_FILE = "/sdcard/dvfel_dither";
constexpr const char* PATTERN_FILE = "/sdcard/dvfel_pattern";constexpr const char* DEMO_FILE = "/sdcard/dvfel_demo";
constexpr const char* AFBC_OUT_FILE = "/sdcard/dvfel_afbc_out";
// test: the base layer and the composed output of the first frame at or after
// this time (ms) to /sdcard/dvfel_{in,out}_<hw|sw>_<pts us>.raw (packed words,
// see dvfel_uapi.h)
constexpr const char* DUMP_FILE = "/sdcard/dvfel_dump";

// ION (Android 4.12+ kernel API); the frame buffers must be physically
// contiguous for the kernel's VICP: the codec_mm CMA heap
constexpr const char* ION_DEV = "/dev/ion";
constexpr const char* ION_HEAP = "codec_mm_cma";
struct ion_allocation_data
{
  uint64_t len;
  uint32_t heap_id_mask;
  uint32_t flags;
  uint32_t fd;
  uint32_t unused;
};
struct ion_heap_data
{
  char name[32];
  uint32_t type;
  uint32_t heap_id;
  uint32_t reserved0;
  uint32_t reserved1;
  uint32_t reserved2;
};
struct ion_heap_query
{
  uint32_t cnt;
  uint32_t reserved0;
  uint64_t heaps;
  uint32_t reserved1;
  uint32_t reserved2;
};
#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_HEAP_QUERY _IOWR(ION_IOC_MAGIC, 8, struct ion_heap_query)

// heap id of ION_HEAP, -1 if missing
int IonHeapId(int ion)
{
  ion_heap_query q{};
  if (ioctl(ion, ION_IOC_HEAP_QUERY, &q) < 0 || !q.cnt)
    return -1;
  std::vector<ion_heap_data> heaps(q.cnt);
  q.heaps = reinterpret_cast<uintptr_t>(heaps.data());
  if (ioctl(ion, ION_IOC_HEAP_QUERY, &q) < 0)
    return -1;
  for (uint32_t i = 0; i < q.cnt && i < heaps.size(); i++)
    if (!strncmp(heaps[i].name, ION_HEAP, sizeof(heaps[i].name)))
      return static_cast<int>(heaps[i].heap_id);
  return -1;
}

int IonAlloc(int ion, int heapId, size_t len)
{
  ion_allocation_data a{};
  a.len = len;
  a.heap_id_mask = 1u << heapId;
  // the Amlogic codec_mm heap allocates only with ION_FLAG_EXTEND_MESON_HEAP;
  // uncached otherwise: written by VICP and the GPU
  a.flags = 1u << 30;
  return ioctl(ion, ION_IOC_ALLOC, &a) < 0 ? -1 : static_cast<int>(a.fd);
}

int ReadIntFile(const char* path, int fallback)
{
  std::ifstream f(path);
  int v = fallback;
  if (f)
    f >> v;
  return f ? v : fallback;
}

double Fixed(int64_t ipart, uint64_t fpart, uint64_t denom)
{
  return static_cast<double>(ipart) + std::ldexp(static_cast<double>(fpart), -static_cast<int>(denom));
}

// original (profile 7) RPU -> composition metadata; nullptr if not usable
std::shared_ptr<felgpu_meta> ParseRpu(const uint8_t* nal, size_t size,
                                      const std::shared_ptr<felgpu_meta>& prev)
{
  DoviRpuOpaque* rpu = dovi_parse_unspec62_nalu(nal, size);
  if (!rpu)
    return nullptr;

  std::shared_ptr<felgpu_meta> meta;
  const DoviRpuDataHeader* hdr = dovi_rpu_get_header(rpu);
  const DoviRpuDataMapping* map = hdr ? dovi_rpu_get_data_mapping(rpu) : nullptr;

  if (hdr && !map && hdr->use_prev_vdr_rpu_flag)
  {
    meta = prev;
  }
  else if (hdr && map && hdr->coefficient_data_type == 0)
  {
    const uint64_t denom = hdr->coefficient_log2_denom;
    meta = std::make_shared<felgpu_meta>();
    felgpu_meta& m = *meta;
    std::memset(&m, 0, sizeof(m));
    m.bl_bits = static_cast<int>(hdr->bl_bit_depth_minus8) + 8;
    m.el_bits = static_cast<int>(hdr->el_bit_depth_minus8) + 8;
    m.use_el = !hdr->disable_residual_flag && map->nlq_method_idc == 0 && map->nlq;

    for (int c = 0; c < 3 && meta; c++)
    {
      const DoviReshapingCurve& cv = map->curves[c];
      const int n = static_cast<int>(cv.num_pivots_minus2) + 2;
      if (n > FELGPU_MAX_PIECES + 1 || cv.pivots.len < static_cast<size_t>(n))
      {
        meta = nullptr;
        break;
      }
      m.npiv[c] = n;
      int acc = 0;
      for (int i = 0; i < n; i++) // pivots are coded as deltas
        m.pivots[c][i] = acc += cv.pivots.data[i];

      for (int k = 0; k < n - 1; k++)
      {
        m.mapping[c][k] = cv.mapping_idc;
        if (cv.mapping_idc == 0 && cv.polynomial)
        {
          const DoviPolynomialCurve& p = *cv.polynomial;
          const int order = static_cast<int>(p.poly_order_minus1.data[k]) + 1;
          for (int j = 0; j <= order && j < 3; j++)
            m.poly[c][k][j] = Fixed(p.poly_coef_int.list[k]->data[j], p.poly_coef.list[k]->data[j], denom);
        }
        else if (cv.mapping_idc == 1 && cv.mmr)
        {
          const DoviMMRCurve& r = *cv.mmr;
          const int order = r.mmr_order_minus1.data[k] + 1;
          m.mmr_order[c][k] = order;
          m.mmr_const[c][k] = Fixed(r.mmr_constant_int.data[k], r.mmr_constant.data[k], denom);
          for (int o = 0; o < order && o < 3; o++)
            for (int t = 0; t < 7; t++)
              m.mmr[c][k][o][t] = Fixed(r.mmr_coef_int.list[k]->list[o]->data[t],
                                        r.mmr_coef.list[k]->list[o]->data[t], denom);
        }
        else
        {
          meta = nullptr;
          break;
        }
      }
      if (meta && m.use_el)
      {
        const DoviRpuDataNlq& q = *map->nlq;
        m.nlq_offset[c] = q.nlq_offset[c];
        m.nlq_slope[c] = Fixed(q.linear_deadzone_slope_int[c], q.linear_deadzone_slope[c], denom);
        m.nlq_thresh[c] = Fixed(q.linear_deadzone_threshold_int[c], q.linear_deadzone_threshold[c], denom);
        m.nlq_max[c] = Fixed(q.vdr_in_max_int[c], q.vdr_in_max[c], denom);
      }
    }
  }

  if (map)
    dovi_rpu_free_data_mapping(map);
  if (hdr)
    dovi_rpu_free_header(hdr);
  dovi_rpu_free(rpu);
  return meta;
}

// NAL unit payload without the emulation prevention bytes (00 00 03)
std::vector<uint8_t> Unescape(const uint8_t* nal, uint32_t size)
{
  std::vector<uint8_t> out;
  out.reserve(size);
  int zeros = 0;
  for (uint32_t i = 0; i < size; i++)
  {
    const uint8_t b = nal[i];
    if (zeros >= 2 && b == 3)
    {
      zeros = 0;
      continue;
    }
    zeros = b ? 0 : zeros + 1;
    out.push_back(b);
  }
  return out;
}

// rpu_data_crc32 of an RPU NAL unit (without start code) as the dvfel kernel
// module reports it: the 32 bits before the RBSP stop bit byte 0x80. The
// decoder hands the kernel the RPU already unescaped, and the module removes
// 00 00 03 once more: a CRC that starts with 03 after two zero bytes comes as
// 00..., so the same is done here.
bool RpuTailCrc(const uint8_t* nal, uint32_t size, uint32_t& crc)
{
  const std::vector<uint8_t> rbsp = Unescape(nal, size);
  uint8_t win[5] = {}, last[5] = {};
  uint32_t n = 0;
  int zeros = 0;
  bool have = false;
  for (const uint8_t b : rbsp)
  {
    if (zeros >= 2 && b == 3)
    {
      zeros = 0;
      continue;
    }
    zeros = b ? 0 : zeros + 1;
    std::memmove(win, win + 1, 4);
    win[4] = b;
    n++;
    if (b && n >= 5)
    {
      std::memcpy(last, win, 5);
      have = true;
    }
  }
  if (!have || last[4] != 0x80)
    return false;
  crc = (uint32_t(last[0]) << 24) | (uint32_t(last[1]) << 16) | (uint32_t(last[2]) << 8) | last[3];
  return true;
}

// HEVC NAL unit types of an Annex-B access unit: the first slice's type
// (-1 without slices) and whether it ends a coded video sequence
void ScanNals(const std::vector<uint8_t>& au, int& vclType, bool& endOfSeq)
{
  vclType = -1;
  endOfSeq = false;
  for (size_t i = 0; i + 4 < au.size(); i++)
  {
    if (au[i] != 0 || au[i + 1] != 0 || au[i + 2] != 1)
      continue;
    // like FFmpeg, skip invalid NAL units (forbidden bit, temporal id 0)
    // and other layers; some EL tracks carry such stray units
    const uint8_t h0 = au[i + 3], h1 = au[i + 4];
    const int layer = ((h0 & 1) << 5) | (h1 >> 3);
    if ((h0 & 0x80) || layer != 0 || (h1 & 7) == 0)
    {
      i += 3;
      continue;
    }
    const int type = (h0 >> 1) & 0x3f;
    if (type < 32 && vclType < 0)
      vclType = type;
    else if (type == 36 || type == 37) // end of sequence / bitstream
      endOfSeq = true;
    i += 3;
  }
}

// NDK functions newer than Kodi's minimum API level (24), loaded at run time
struct NdkImageApi
{
  struct NativeHandle
  {
    int version, numFds, numInts;
    int data[0];
  };
  media_status_t (*newWithUsage)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**);
  media_status_t (*getHardwareBuffer)(const AImage*, AHardwareBuffer**);
  const NativeHandle* (*getNativeHandle)(const AHardwareBuffer*); // LL-NDK

  bool Load()
  {
    void* media = dlopen("libmediandk.so", RTLD_NOW);
    void* window = dlopen("libnativewindow.so", RTLD_NOW);
    if (!media || !window)
      return false;
    newWithUsage = reinterpret_cast<decltype(newWithUsage)>(dlsym(media, "AImageReader_newWithUsage"));
    getHardwareBuffer =
        reinterpret_cast<decltype(getHardwareBuffer)>(dlsym(media, "AImage_getHardwareBuffer"));
    getNativeHandle = reinterpret_cast<decltype(getNativeHandle)>(
        dlsym(window, "AHardwareBuffer_getNativeHandle"));
    return newWithUsage && getHardwareBuffer && getNativeHandle;
  }
};
constexpr int32_t AIMAGE_FORMAT_PRIVATE_ = 0x22;
constexpr uint64_t AHB_USAGE_GPU_SAMPLED_IMAGE = 1ULL << 8;

// std::map lookup of the entry closest to pts, within the tolerance
template<typename T>
typename std::map<int64_t, T>::iterator FindPts(std::map<int64_t, T>& m, int64_t pts)
{
  auto it = m.lower_bound(pts - PTS_TOLERANCE_US);
  if (it != m.end() && std::llabs(it->first - pts) <= PTS_TOLERANCE_US)
    return it;
  return m.end();
}
} // namespace

CAndroidDvFel::~CAndroidDvFel()
{
  Stop();
}

bool CAndroidDvFel::IsAvailable()
{
  return access(DVFEL_DEV, R_OK | W_OK) == 0 && access(ION_DEV, R_OK) == 0;
}

namespace
{
// run as root by InstallModuleAsync: $1 = the bundled module. The Dune HD
// firmware (Dune and R_volution players) runs every executable in
// <FS_PREFIX>/config/boot as root at boot (binit.sh). One module build for all
// firmware versions: their kernel only warns about symbol CRC mismatches
// ("disagrees about version of symbol") and loads it anyway; the warnings go
// to the log (a changed structure behind them would be the first suspect).
constexpr const char* INSTALL_SCRIPT = R"SH(#!/system/bin/sh
KO="$1"
FSP=$(sed -n 's/^FSP=//p' /system/xbin/dunehd/init 2>/dev/null | head -1)
[ -n "$FSP" ] || FSP=/data/data/com.dunehd.app
grep -q 'config/boot' /system/dunehd/firmware/scripts/binit.sh 2>/dev/null ||
  { echo "no Dune HD firmware boot hook (binit.sh)"; exit 2; }
D=$FSP/config
[ -d "$D" ] || { echo "$D missing"; exit 2; }
echo "firmware $(grep -h firmware_version "$D/last_fw_info.txt" 2>/dev/null), $(getprop ro.product.model), kernel $(uname -r)"
mkdir -p "$D/dvfel" "$D/boot" && chmod 755 "$D/dvfel" "$D/boot" || exit 3
INST="$D/dvfel/dvfel.ko"

# an older module: replaced (not in use at Kodi start)
if [ -d /sys/module/dvfel ]; then
  rmmod dvfel || { echo "the loaded module is in use, try again after a reboot"; exit 4; }
fi
b=$(dmesg | grep -c 'dvfel: disagrees about version')
if ! insmod "$KO"; then
  echo "insmod failed:"
  dmesg | grep -i dvfel | tail -3
  [ -f "$INST" ] && insmod "$INST" 2>/dev/null && echo "the installed module is loaded again"
  exit 5
fi
a=$(dmesg | grep -c 'dvfel: disagrees about version')
if [ "$a" -gt "$b" ]; then
  echo "note: symbol versions differ from the build firmware (loaded anyway):"
  dmesg | grep 'dvfel: disagrees about version' | tail -n $((a - b)) | head -10
fi
if ! cmp -s "$KO" "$INST"; then
  cp "$KO" "$INST.tmp" && chmod 644 "$INST.tmp" && mv "$INST.tmp" "$INST" || exit 6
  echo "module installed to $INST"
fi

cat > "$D/boot/dvfel.sh.tmp" <<EOS
#!/system/bin/sh
# Kodi FEL: Dolby Vision FEL composition kernel module (dvfel)
insmod $INST || exit 1
# ueventd creates the device node asynchronously
i=0
while [ ! -e /dev/dvfel ] && [ \$i -lt 50 ]; do sleep 0.1; i=\$((i + 1)); done
chmod 666 /dev/dvfel
EOS
if cmp -s "$D/boot/dvfel.sh.tmp" "$D/boot/dvfel.sh"; then
  rm -f "$D/boot/dvfel.sh.tmp"
else
  mv "$D/boot/dvfel.sh.tmp" "$D/boot/dvfel.sh" && echo "boot script installed"
fi
chmod 755 "$D/boot/dvfel.sh"
i=0
while [ ! -e /dev/dvfel ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
chmod 666 /dev/dvfel || exit 7
echo "dvfel $(cat /sys/module/dvfel/version 2>/dev/null) ready"
)SH";
} // namespace

namespace
{
// the Dune firmware's switch for su (its sud daemon): "" (default) or "1" on
constexpr const char* ROOT_PROP = "persist.vendor.root_access";
constexpr const char* NOTIFY_HEADING = "Dolby Vision FEL";

// runs a shell command; output lines to the log when logPrefix is set
int Run(const std::string& cmd, const char* logPrefix = nullptr, std::string* out = nullptr)
{
  FILE* p = popen((cmd + " 2>&1").c_str(), "r");
  if (!p)
    return -1;
  char line[256];
  while (fgets(line, sizeof(line), p))
  {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
      s.pop_back();
    if (logPrefix)
      CLog::Log(LOGINFO, "CAndroidDvFel: {}: {}", logPrefix, s);
    if (out)
      *out += s;
  }
  return pclose(p);
}

bool SuWorks()
{
  return Run("timeout 10 su -c true") == 0;
}

std::string ReadLine(const std::string& path)
{
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

// the GUI can show dialogs (startup finished)
bool WaitForGui()
{
  for (int i = 0; i < 600; i++)
  {
    CGUIComponent* gui = CServiceBroker::GetGUI();
    if (g_application.IsInitialized() && gui && gui->GetWindowManager().Initialized())
    {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return false;
}
} // namespace

namespace
{
// remembers a "No" to the root question for one module version
std::string DeclinedFile(const std::string& dir = "")
{
  static std::string path;
  static std::mutex lock;
  std::lock_guard<std::mutex> l(lock);
  if (!dir.empty())
    path = dir + "/dvfel_root_declined";
  return path;
}
} // namespace

void CAndroidDvFel::OnModuleMissing()
{
  static std::atomic<bool> told{false};
  const std::string declined = DeclinedFile();
  if (told.exchange(true) || declined.empty())
    return;
  // ask again at the next start
  unlink(declined.c_str());
  CGUIDialogKaiToast::QueueNotification(
      CGUIDialogKaiToast::Info, NOTIFY_HEADING,
      "Restart Kodi FEL to install the FEL module", 10000);
}

void CAndroidDvFel::InstallModuleAsync(const std::string& dir)
{
  DeclinedFile(dir);
  std::thread([dir] {
    // the bundled module or a newer one (another Kodi FEL build installed it;
    // versions are dates, YYYY.MM.DD) is loaded already: nothing to do, no
    // root needed. Never replace a newer module with an older one.
    const std::string loaded = ReadLine("/sys/module/dvfel/version");
    if (IsAvailable() && !loaded.empty() && loaded >= std::string(DVFEL_VERSION))
    {
      CLog::Log(LOGINFO, "CAndroidDvFel: kernel module {} loaded (bundled {})", loaded,
                DVFEL_VERSION);
      return;
    }

    const std::string script = dir + "/dvfel_install.sh";
    const std::string declinedFile = DeclinedFile(dir);
    const std::string ko = dir + "/dvfel.ko";
    if (!XFILE::CFile::Copy("special://xbmc/system/dvfel/dvfel.ko", ko))
    {
      CLog::Log(LOGINFO, "CAndroidDvFel: no bundled kernel module");
      return;
    }
    {
      std::ofstream f(script, std::ios::trunc);
      f << INSTALL_SCRIPT;
      if (!f)
      {
        CLog::Log(LOGERROR, "CAndroidDvFel: cannot write {}", script);
        return;
      }
    }
    if (access("/system/bin/su", X_OK) != 0 && access("/system/xbin/su", X_OK) != 0)
    {
      CLog::Log(LOGINFO, "CAndroidDvFel: no su, the kernel module cannot be installed");
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, NOTIFY_HEADING,
                                            "Kernel module not installed: no su", 10000);
      return;
    }

    // su answers only while the firmware's root access is on: ask the user
    // to switch it on for the installation (an app may set the property on
    // this permissive firmware), then restore the previous state
    std::string previous;
    bool switched = false;
    if (!SuWorks())
    {
      if (ReadLine(declinedFile) == DVFEL_VERSION)
      {
        CLog::Log(LOGINFO, "CAndroidDvFel: root access off, installation declined earlier");
        return;
      }
      if (!WaitForGui())
        return;
      bool cancelled = false;
      const bool yes = CGUIDialogYesNo::ShowAndGetInput(
          NOTIFY_HEADING,
          "To play Dolby Vision FEL, Kodi FEL installs a kernel module once. This needs root "
          "access, which is switched off on this player.",
          "Switch root access on for the installation? It is switched off again afterwards.", "",
          cancelled, "No", "Install", 0);
      if (!yes)
      {
        std::ofstream(declinedFile, std::ios::trunc) << DVFEL_VERSION << '\n';
        CLog::Log(LOGINFO, "CAndroidDvFel: root access off, installation declined");
        CGUIDialogKaiToast::QueueNotification(
            CGUIDialogKaiToast::Info, NOTIFY_HEADING,
            "Not installed: FEL plays without the enhancement layer", 8000);
        return;
      }
      Run(std::string("getprop ") + ROOT_PROP, nullptr, &previous);
      Run(std::string("setprop ") + ROOT_PROP + " 1", "root access");
      switched = true;
      bool ok = false;
      for (int i = 0; i < 20 && !ok; i++)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ok = SuWorks();
      }
      CLog::Log(LOGINFO, "CAndroidDvFel: root access switched on (was '{}'): {}", previous,
                ok ? "su works" : "su still unavailable");
      if (!ok)
      {
        Run(std::string("setprop ") + ROOT_PROP + " '" + previous + "'", "root access");
        CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, NOTIFY_HEADING,
                                              "Root access could not be switched on", 10000);
        return;
      }
    }

    const int rc = Run("timeout 90 su -c 'sh " + script + " " + ko + "'", "module install");
    if (switched)
    {
      Run(std::string("setprop ") + ROOT_PROP + " '" + previous + "'", "root access");
      CLog::Log(LOGINFO, "CAndroidDvFel: root access restored to '{}'", previous);
    }
    const bool available = IsAvailable();
    CLog::Log(rc == 0 ? LOGINFO : LOGWARNING,
              "CAndroidDvFel: module install {} (status {}); FEL composition {}",
              rc == 0 ? "done" : "failed", rc, available ? "available" : "not available");
    if (available)
    {
      unlink(declinedFile.c_str());
      if (switched)
        CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, NOTIFY_HEADING,
                                              "Kernel module installed", 6000);
    }
    else
    {
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, NOTIFY_HEADING,
                                            "Kernel module not installed (see kodi.log)", 10000);
    }
  }).detach();
}

bool CAndroidDvFel::PendingIsFel() const
{
  if (m_pendingRpu.empty())
    return false;
  DoviRpuOpaque* rpu = dovi_parse_unspec62_nalu(m_pendingRpu.data(), m_pendingRpu.size());
  if (!rpu)
    return false;
  const DoviRpuDataHeader* hdr = dovi_rpu_get_header(rpu);
  const bool fel = hdr && hdr->guessed_profile == 7 && hdr->el_type && !strcmp(hdr->el_type, "FEL");
  if (hdr)
    dovi_rpu_free_header(hdr);
  dovi_rpu_free(rpu);
  return fel;
}

bool CAndroidDvFel::Start(int width, int height)
{
  if (m_started)
    return true;
  if (width <= 0 || height <= 0 || (width & 3) || (height & 3))
    return false;

  m_width = width;
  m_height = height;
  m_elWidth = width / 2;
  m_elHeight = height / 2;
  m_hwEl = ReadIntFile(SW_EL_FILE, 0) != 1;
  for (int i = 0; i < EL_POOL; i++)
  {
    m_elPoolFd[i] = -1;
    m_elPoolBusy[i] = false;
  }
  m_stop = false;
  m_noElBefore = INT64_MIN;
  m_noPairLogged = 0;
  m_noCrcLogged = 0;

  std::promise<bool> ready;
  std::future<bool> ok = ready.get_future();
  m_gpuThread = std::thread(&CAndroidDvFel::GpuThread, this, std::move(ready));
  if (!ok.get())
  {
    m_gpuThread.join();
    for (int i = 0; i < EL_POOL; i++)
    {
      if (m_elPoolFd[i] >= 0)
        close(m_elPoolFd[i]);
      m_elPoolFd[i] = -1;
    }
    CLog::Log(LOGERROR, "CAndroidDvFel: GPU compositor failed to start, FEL composition disabled");
    return false;
  }
  const bool hwEl = m_hwEl;
  m_elThread = std::thread(hwEl ? &CAndroidDvFel::HwElThread : &CAndroidDvFel::ElThread, this);
  m_started = true;
  CLog::Log(LOGINFO, "CAndroidDvFel: FEL composition started for {}x{} ({} EL decoder)", width,
            height, hwEl ? "hardware" : "software");
  return true;
}

void CAndroidDvFel::Stop()
{
  if (!m_started && !m_gpuThread.joinable())
    return;

  m_stop = true;
  m_elCond.notify_all();
  m_readyCond.notify_all();
  if (m_elThread.joinable())
    m_elThread.join();
  if (m_gpuThread.joinable())
    m_gpuThread.join();

  if (m_started)
  {
    const uint64_t n = m_composed;
    std::vector<float>& t = m_gpuMs;
    std::sort(t.begin(), t.end());
    auto pct = [&t](double p) { return t.empty() ? 0.0f : t[static_cast<size_t>(p * (t.size() - 1))]; };
    const auto slow = std::count_if(t.begin(), t.end(), [](float ms) { return ms > 41.7f; });
    CLog::Log(LOGINFO,
              "CAndroidDvFel: stopped, {} frames composed (gpu avg {:.1f} ms, median {:.1f}, p95 "
              "{:.1f}, max {:.1f}, {} over 41.7 ms), {} shown without EL ({} without waiting, {} with the "
              "mapping only), {} EL frames decoded",
              n, n ? m_gpuMsSum / n : 0.0, pct(0.5), pct(0.95), pct(1.0), slow,
              m_passthrough.load(), m_noElSkipped.load(), m_mappedOnly.load(), m_elDecoded.load());
    t.clear();
  }

  std::lock_guard<std::mutex> lock(m_lock);
  for (auto& f : m_elFrames)
    ReleaseElFrame(f.second);
  m_elFrames.clear();
  m_elQueue.clear();
  m_crcByPts.clear();
  m_metaByPts.clear();
  // the EL pool, used by both threads
  for (int i = 0; i < EL_POOL; i++)
  {
    if (m_elPoolFd[i] >= 0)
      close(m_elPoolFd[i]);
    m_elPoolFd[i] = -1;
  }
  m_started = false;
}

void CAndroidDvFel::ReleaseElFrame(ElFrame& el)
{
  av_frame_free(&el.frame);
  if (el.buf >= 0 && el.buf < EL_POOL)
  {
    m_elPoolBusy[el.buf] = false;
    m_elCond.notify_all();
  }
  el.buf = -1;
  el.meta.reset();
}

void CAndroidDvFel::Reset()
{
  std::lock_guard<std::mutex> lock(m_lock);
  m_elQueue.clear();
  m_elQueue.push_back({0, {}, nullptr, true});
  m_crcByPts.clear();
  m_metaByPts.clear();
  m_lastJobPts = INT64_MIN;
  m_noPairLogged = 0;
  m_noCrcLogged = 0;
  m_pendingRpu.clear();
  m_pendingEl.clear();
  m_pendingCrcValid = false;
  m_elCond.notify_all();
}

void CAndroidDvFel::OnDoviRpu(const uint8_t* nal, uint32_t size)
{
  m_pendingRpu.assign(nal, nal + size);
}

void CAndroidDvFel::OnDoviElNal(const uint8_t* nal, uint32_t size)
{
  static const uint8_t startCode[] = {0, 0, 0, 1};
  m_pendingEl.insert(m_pendingEl.end(), startCode, startCode + 4);
  m_pendingEl.insert(m_pendingEl.end(), nal, nal + size);
}

void CAndroidDvFel::OnDoviRpuSent(const uint8_t* nal, uint32_t size)
{
  m_pendingCrcValid = RpuTailCrc(nal, size, m_pendingCrc);
}

void CAndroidDvFel::DropPacket()
{
  m_pendingRpu.clear();
  m_pendingEl.clear();
  m_pendingCrcValid = false;
}

void CAndroidDvFel::CommitPacket(double pts)
{
  if (!m_started || pts < 0)
  {
    DropPacket();
    return;
  }
  const int64_t key = static_cast<int64_t>(pts);

  std::shared_ptr<felgpu_meta> meta;
  if (!m_pendingRpu.empty())
    meta = ParseRpu(m_pendingRpu.data(), m_pendingRpu.size(), m_lastMeta);
  if (meta)
    m_lastMeta = meta;

  std::lock_guard<std::mutex> lock(m_lock);
  if (m_pendingCrcValid)
  {
    m_crcByPts[key] = m_pendingCrc;
    while (m_crcByPts.size() > MAX_CRC)
      m_crcByPts.erase(m_crcByPts.begin());
  }
  if (meta)
  {
    // for frames that get no EL (composed with the mapping only)
    m_metaByPts[key] = meta;
    while (m_metaByPts.size() > MAX_META)
      m_metaByPts.erase(m_metaByPts.begin());
  }
  if (!m_pendingEl.empty() && m_elQueue.size() < MAX_EL_QUEUE)
  {
    // the RPU closes the EL access unit as in a demuxed EL stream: without a
    // NAL unit after the last slice, the Amlogic HEVC decoder's back end times
    // out (200 ms) on some pictures, and the pictures referencing them are lost
    if (!m_pendingRpu.empty())
    {
      static const uint8_t startCode[] = {0, 0, 0, 1};
      m_pendingEl.insert(m_pendingEl.end(), startCode, startCode + 4);
      m_pendingEl.insert(m_pendingEl.end(), m_pendingRpu.begin(), m_pendingRpu.end());
    }
    m_elQueue.push_back({key, std::move(m_pendingEl), meta, false});
    m_elCond.notify_all();
  }
  m_pendingEl.clear();
  m_pendingRpu.clear();
  m_pendingCrcValid = false;
}

int64_t CAndroidDvFel::PtsForRpuCrc(uint32_t crc)
{
  std::lock_guard<std::mutex> lock(m_lock);
  // frames come in display order, the map is in pts order: the frame is
  // normally the first pending packet; earlier ones it skips were dropped
  size_t n = 0;
  for (auto it = m_crcByPts.begin(); it != m_crcByPts.end() && n < CRC_SEARCH; ++it, n++)
  {
    if (it->second != crc)
      continue;
    const int64_t pts = it->first;
    m_crcByPts.erase(m_crcByPts.begin(), std::next(it));
    return pts;
  }
  return -1;
}

void CAndroidDvFel::ElThread()
{
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
  AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!ctx)
    return;
  // frame threads on every core: the hardware decoder leaves them idle
  ctx->thread_count = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 3u, 8u));
  ctx->thread_type = FF_THREAD_FRAME;
  if (avcodec_open2(ctx, codec, nullptr) < 0)
  {
    CLog::Log(LOGERROR, "CAndroidDvFel: cannot open the EL decoder");
    avcodec_free_context(&ctx);
    return;
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  // display order pairing, see CAMLDvFel::ElThread
  std::multiset<int64_t> slots;
  std::map<int64_t, std::shared_ptr<felgpu_meta>> metas;
  int skipped = 0;
  bool reordered = false;
  int64_t lastLabel = INT64_MIN;
  bool needIrap = true, skipRasl = false;

  while (!m_stop)
  {
    ElPacket in;
    {
      std::unique_lock<std::mutex> lock(m_lock);
      // pace decoding by the display: stay at most EL_LEAD_US ahead of the
      // last requested frame (before the first request: a few frames)
      auto paced = [this] {
        if (m_elFrames.size() >= MAX_EL_FRAMES)
          return false;
        if (m_lastJobPts == INT64_MIN)
          return m_elFrames.size() < 12;
        return m_elFrames.empty() || m_elFrames.rbegin()->first < m_lastJobPts + EL_LEAD_US;
      };
      m_elCond.wait(lock, [&] {
        return m_stop || (!m_elQueue.empty() && (m_elQueue.front().flush || paced()));
      });
      if (m_stop)
        break;
      in = std::move(m_elQueue.front());
      m_elQueue.pop_front();
    }

    if (in.flush)
    {
      avcodec_flush_buffers(ctx);
      slots.clear();
      metas.clear();
      skipped = 0;
      lastLabel = INT64_MIN;
      needIrap = true;
      skipRasl = false;
      std::lock_guard<std::mutex> lock(m_lock);
      for (auto& f : m_elFrames)
        ReleaseElFrame(f.second);
      m_elFrames.clear();
      m_noElBefore = INT64_MAX; // until the first IRAP of the EL
      continue;
    }

    int type;
    bool endOfSeq;
    ScanNals(in.data, type, endOfSeq);
    if (type < 0)
      continue;
    bool decode = true;
    if (type >= 16 && type <= 23) // IRAP: BLA 16-18, IDR 19-20, CRA 21
    {
      skipRasl = type <= 18 || (type == 21 && needIrap);
      if (needIrap)
      {
        // frames shown before it (skipped leading pictures) get no EL
        std::lock_guard<std::mutex> lock(m_lock);
        m_noElBefore = in.pts;
      }
      needIrap = false;
    }
    else if (needIrap || (skipRasl && (type == 8 || type == 9)))
    {
      decode = false;
    }
    if (endOfSeq)
      needIrap = true;

    slots.insert(in.pts);
    if (!decode)
    {
      skipped++;
      continue;
    }
    metas[in.pts] = std::move(in.meta);
    while (metas.size() > 2 * MAX_EL_FRAMES) // never output, e.g. broken pictures
      metas.erase(metas.begin());

    if (av_new_packet(pkt, static_cast<int>(in.data.size())) < 0)
      continue;
    std::memcpy(pkt->data, in.data.data(), in.data.size());
    pkt->pts = in.pts;
    int ret = avcodec_send_packet(ctx, pkt);
    av_packet_unref(pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
      continue;

    while (avcodec_receive_frame(ctx, frame) == 0)
    {
      for (; skipped > 0 && !slots.empty(); skipped--)
        slots.erase(slots.begin());
      if (slots.empty())
      {
        av_frame_unref(frame);
        continue;
      }
      // output is in display order: the earliest pts is this picture's slot
      const int64_t slot = *slots.begin();
      slots.erase(slots.begin());
      // frame->pts is the pts of the packet that carried the picture
      if (!reordered && lastLabel != INT64_MIN && frame->pts < lastLabel)
      {
        reordered = true;
        CLog::Log(LOGINFO, "CAndroidDvFel: the EL picture order differs from the base layer, "
                           "pairing by display order");
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto it = m_elFrames.lower_bound(slot); it != m_elFrames.end();)
        {
          ReleaseElFrame(it->second);
          it = m_elFrames.erase(it);
        }
      }
      lastLabel = frame->pts;
      const int64_t pts = reordered ? slot : frame->pts;
      std::shared_ptr<felgpu_meta> meta;
      // the RPU travels with the EL picture
      auto m = metas.find(frame->pts);
      if (m != metas.end())
      {
        meta = std::move(m->second);
        metas.erase(m);
      }
      if (!meta || frame->format != AV_PIX_FMT_YUV420P10 || frame->width * 2 != m_width ||
          frame->height * 2 != m_height)
      {
        av_frame_unref(frame);
        continue;
      }
      AVFrame* keep = av_frame_clone(frame);
      av_frame_unref(frame);
      keep->pts = pts;
      std::lock_guard<std::mutex> lock(m_lock);
      // only frames the display has passed may go; never drop a future one
      while (m_lastJobPts != INT64_MIN && m_elFrames.size() >= MAX_EL_FRAMES &&
             m_elFrames.begin()->first < m_lastJobPts - PTS_TOLERANCE_US)
      {
        ReleaseElFrame(m_elFrames.begin()->second);
        m_elFrames.erase(m_elFrames.begin());
      }
      auto& entry = m_elFrames[pts];
      ReleaseElFrame(entry);
      entry.frame = keep;
      entry.meta = std::move(meta);
      m_elDecoded++;
      m_readyCond.notify_all();
    }
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
}

void CAndroidDvFel::HwElThread()
{
  NdkImageApi api{};
  AImageReader* reader = nullptr;
  ANativeWindow* window = nullptr;
  AMediaCodec* codec = nullptr;
  const int dev = open(DVFEL_DEV, O_RDWR | O_CLOEXEC);

  bool ok = api.Load() && dev >= 0;
  if (ok)
    ok = api.newWithUsage(m_elWidth, m_elHeight, AIMAGE_FORMAT_PRIVATE_, AHB_USAGE_GPU_SAMPLED_IMAGE,
                          HW_EL_IMAGES, &reader) == AMEDIA_OK &&
         AImageReader_getWindow(reader, &window) == AMEDIA_OK;
  if (ok)
  {
    codec = AMediaCodec_createCodecByName(HW_EL_CODEC);
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, m_elWidth);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, m_elHeight);
    ok = codec && AMediaCodec_configure(codec, fmt, window, nullptr, 0) == AMEDIA_OK &&
         AMediaCodec_start(codec) == AMEDIA_OK;
    AMediaFormat_delete(fmt);
  }
  if (!ok)
  {
    CLog::Log(LOGERROR, "CAndroidDvFel: cannot start the hardware EL decoder {}, software EL decoding",
              HW_EL_CODEC);
    if (codec)
      AMediaCodec_delete(codec);
    if (reader)
      AImageReader_delete(reader);
    if (dev >= 0)
      close(dev);
    m_hwEl = false;
    ElThread();
    return;
  }
  CLog::Log(LOGINFO, "CAndroidDvFel: hardware EL decoder {} started ({}x{})", HW_EL_CODEC,
            m_elWidth, m_elHeight);

  // display order pairing and IRAP handling as in ElThread
  std::multiset<int64_t> slots;
  std::map<int64_t, std::shared_ptr<felgpu_meta>> metas;
  int skipped = 0;
  bool reordered = false;
  int64_t lastLabel = INT64_MIN;
  bool needIrap = true, skipRasl = false;
  uint64_t importErrors = 0, importUs = 0, imports = 0;
  uint64_t staleImages = 0, lateImages = 0;
  FILE* elDump = access("/sdcard/dvfel_eldump", F_OK) == 0 ? fopen("/sdcard/dvfel_eldump.bin", "wb") : nullptr;
  int elDumped = 0;
  int truncated = 0;
  m_elInputs = 0;
  m_elOutputs = 0;

  // a free EL pool buffer (m_lock held): frames the display has passed may go
  auto freeBuf = [this]() -> int {
    while (true)
    {
      for (int i = 0; i < EL_POOL; i++)
        if (!m_elPoolBusy[i])
          return i;
      if (m_lastJobPts == INT64_MIN || m_elFrames.empty() ||
          m_elFrames.begin()->first >= m_lastJobPts - PTS_TOLERANCE_US)
        return -1;
      ReleaseElFrame(m_elFrames.begin()->second);
      m_elFrames.erase(m_elFrames.begin());
    }
  };

  // takes the decoded pictures out of the decoder, always: an output the
  // decoder cannot hand over stalls its back end, which times out (200 ms)
  // and marks the picture and everything referencing it as broken until the
  // next IRAP. With the pool full (should not happen with the pacing), the
  // new picture (the farthest ahead) is dropped instead.
  auto drain = [&](int64_t timeoutUs) -> bool {
    while (!m_stop)
    {
      int buf;
      {
        std::lock_guard<std::mutex> lock(m_lock);
        buf = freeBuf();
        if (buf >= 0)
          m_elPoolBusy[buf] = true;
      }
      auto unbusy = [&] {
        if (buf < 0)
          return;
        std::lock_guard<std::mutex> lock(m_lock);
        m_elPoolBusy[buf] = false;
      };

      AMediaCodecBufferInfo info;
      const ssize_t o = AMediaCodec_dequeueOutputBuffer(codec, &info, timeoutUs);
      timeoutUs = 0;
      if (o < 0 || (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM))
      {
        if (o >= 0)
          AMediaCodec_releaseOutputBuffer(codec, o, false);
        unbusy();
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
          continue;
        return true;
      }
      AMediaCodec_releaseOutputBuffer(codec, o, buf >= 0);
      m_elOutputs++;
      m_elLastOut = info.presentationTimeUs;
      if (buf < 0 && m_elOverflow++ < 10)
        CLog::Log(LOGINFO, "CAndroidDvFel: EL pool full, picture {} us dropped",
                  info.presentationTimeUs);

      // the picture as an image of the reader (its timestamp is the buffer's
      // presentation time): older images arrived late for an earlier output,
      // whose picture is lost; they must not be taken for this one
      AImage* image = nullptr;
      for (int i = 0; buf >= 0 && i < HW_EL_IMAGE_WAIT_MS && !m_stop && !image; i++)
      {
        if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK)
        {
          image = nullptr;
          usleep(1000);
          continue;
        }
        int64_t ts = 0;
        AImage_getTimestamp(image, &ts);
        if (ts / 1000 != info.presentationTimeUs)
        {
          if (staleImages++ < 10)
            CLog::Log(LOGINFO, "CAndroidDvFel: EL image {} us is not the output {} us, dropped",
                      ts / 1000, info.presentationTimeUs);
          AImage_delete(image);
          image = nullptr;
        }
      }
      if (buf >= 0 && !image && lateImages++ < 10)
        CLog::Log(LOGINFO, "CAndroidDvFel: no EL image for output {} us", info.presentationTimeUs);
      bool imported = false;
      if (image)
      {
        AHardwareBuffer* hb = nullptr;
        const NdkImageApi::NativeHandle* h = nullptr;
        if (api.getHardwareBuffer(image, &hb) == AMEDIA_OK && hb)
          h = api.getNativeHandle(hb);
        if (h && h->numFds > 0)
        {
          dvfel_el_import im{};
          im.src_fd = h->data[0];
          im.dst_fd = m_elPoolFd[buf];
          im.width = m_elWidth;
          im.height = m_elHeight;
          imported = ioctl(dev, DVFEL_IOC_EL_IMPORT, &im) == 0;
          if (imported)
          {
            imports++;
            importUs += im.us;
          }
          else if (importErrors++ < 5)
            CLog::Log(LOGERROR, "CAndroidDvFel: DVFEL_IOC_EL_IMPORT failed: {}", strerror(errno));
        }
        AImage_delete(image);
      }

      // output is in display order: the earliest pending pts is this picture's slot
      for (; skipped > 0 && !slots.empty(); skipped--)
        slots.erase(slots.begin());
      if (slots.empty())
      {
        unbusy();
        continue;
      }
      const int64_t slot = *slots.begin();
      slots.erase(slots.begin());
      // the pts of the packet that carried the picture
      const int64_t label = info.presentationTimeUs;
      std::unique_lock<std::mutex> lock(m_lock);
      if (!reordered && lastLabel != INT64_MIN && label < lastLabel)
      {
        reordered = true;
        CLog::Log(LOGINFO, "CAndroidDvFel: the EL picture order differs from the base layer, "
                           "pairing by display order");
        for (auto it = m_elFrames.lower_bound(slot); it != m_elFrames.end();)
        {
          ReleaseElFrame(it->second);
          it = m_elFrames.erase(it);
        }
      }
      lastLabel = label;
      const int64_t pts = reordered ? slot : label;
      std::shared_ptr<felgpu_meta> meta;
      // the RPU travels with the EL picture
      auto m = metas.find(label);
      if (m != metas.end())
      {
        meta = std::move(m->second);
        metas.erase(m);
      }
      if (!imported || !meta)
      {
        if (buf >= 0)
          m_elPoolBusy[buf] = false;
        continue;
      }
      auto& entry = m_elFrames[pts];
      ReleaseElFrame(entry);
      entry.buf = buf;
      entry.meta = std::move(meta);
      m_elDecoded++;
      m_readyCond.notify_all();
    }
    return true;
  };

  while (!m_stop)
  {
    ElPacket in;
    bool have = false;
    {
      std::unique_lock<std::mutex> lock(m_lock);
      // pace decoding by the display, see ElThread; the pool bounds it too
      auto paced = [this] {
        // the pictures still in the decoder need pool buffers too
        const int64_t inDecoder = static_cast<int64_t>(m_elInputs) - static_cast<int64_t>(m_elOutputs);
        if (static_cast<int64_t>(m_elFrames.size()) + std::clamp<int64_t>(inDecoder, 0, EL_IN_DECODER_MAX) >= EL_POOL)
          return false;
        return m_lastJobPts == INT64_MIN || m_elFrames.empty() ||
               m_elFrames.rbegin()->first < m_lastJobPts + EL_LEAD_US;
      };
      m_elCond.wait_for(lock, std::chrono::milliseconds(5), [&] {
        return m_stop || (!m_elQueue.empty() && (m_elQueue.front().flush || paced()));
      });
      if (m_stop)
        break;
      if (!m_elQueue.empty() && (m_elQueue.front().flush || paced()))
      {
        in = std::move(m_elQueue.front());
        m_elQueue.pop_front();
        have = true;
      }
    }
    if (!have)
    {
      drain(0);
      continue;
    }

    if (in.flush)
    {
      AMediaCodec_flush(codec);
      m_elOutputs = m_elInputs.load(); // the pictures in the decoder are gone
      // pictures released to the reader before the flush
      AImage* image = nullptr;
      while (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK)
        AImage_delete(image);
      slots.clear();
      metas.clear();
      skipped = 0;
      lastLabel = INT64_MIN;
      needIrap = true;
      skipRasl = false;
      std::lock_guard<std::mutex> lock(m_lock);
      for (auto& f : m_elFrames)
        ReleaseElFrame(f.second);
      m_elFrames.clear();
      m_noElBefore = INT64_MAX; // until the first IRAP of the EL
      continue;
    }

    int type;
    bool endOfSeq;
    ScanNals(in.data, type, endOfSeq);
    if (type < 0)
      continue;
    bool decode = true;
    if (type >= 16 && type <= 23) // IRAP: BLA 16-18, IDR 19-20, CRA 21
    {
      skipRasl = type <= 18 || (type == 21 && needIrap);
      if (needIrap)
      {
        // frames shown before it (skipped leading pictures) get no EL
        std::lock_guard<std::mutex> lock(m_lock);
        m_noElBefore = in.pts;
      }
      needIrap = false;
    }
    else if (needIrap || (skipRasl && (type == 8 || type == 9)))
    {
      decode = false;
    }
    if (endOfSeq)
      needIrap = true;

    slots.insert(in.pts);
    if (!decode)
    {
      skipped++;
      continue;
    }
    metas[in.pts] = std::move(in.meta);
    while (metas.size() > 4 * EL_POOL) // never output, e.g. broken pictures
      metas.erase(metas.begin());

    // an input buffer; decoded pictures are taken out meanwhile
    ssize_t idx;
    while ((idx = AMediaCodec_dequeueInputBuffer(codec, 0)) < 0 && !m_stop)
    {
      if (!drain(5000))
      {
        std::unique_lock<std::mutex> lock(m_lock);
        m_elCond.wait_for(lock, std::chrono::milliseconds(5));
      }
    }
    if (idx < 0)
      break;
    size_t cap = 0;
    uint8_t* p = AMediaCodec_getInputBuffer(codec, idx, &cap);
    const size_t size = std::min(cap, in.data.size());
    if (in.data.size() > cap && truncated++ < 10)
      CLog::Log(LOGWARNING, "CAndroidDvFel: EL picture {} us of {} bytes truncated to the input buffer ({} bytes)",
                in.pts, in.data.size(), cap);
    if (p)
      std::memcpy(p, in.data.data(), size);
    AMediaCodec_queueInputBuffer(codec, idx, 0, p ? size : 0, static_cast<uint64_t>(in.pts), 0);
    if (elDump && elDumped < 3000)
    {
      // test: /sdcard/dvfel_eldump exists: the packets as fed, each with a 4-byte size and 8-byte pts
      const uint32_t n = static_cast<uint32_t>(size);
      const int64_t t = in.pts;
      fwrite(&n, 4, 1, elDump);
      fwrite(&t, 8, 1, elDump);
      fwrite(in.data.data(), 1, size, elDump);
      if (++elDumped == 3000)
        fclose(elDump), elDump = nullptr;
    }
    m_elInputs++;
    drain(0);
  }

  CLog::Log(LOGINFO, "CAndroidDvFel: hardware EL decoder stopped, {} pictures imported (VICP avg "
                     "{:.1f} ms), {} import errors",
            imports, imports ? importUs / 1000.0 / imports : 0.0, importErrors);
  AMediaCodec_stop(codec);
  if (elDump)
    fclose(elDump);
  AMediaCodec_delete(codec);
  AImageReader_delete(reader);
  close(dev);
}

bool CAndroidDvFel::WaitForFrame(int64_t pts, ElFrame& el)
{
  std::unique_lock<std::mutex> lock(m_lock);
  // the display reached pts: let the paced EL decoder run ahead of it
  m_lastJobPts = pts;
  while (!m_elFrames.empty() && m_elFrames.begin()->first < pts - PTS_TOLERANCE_US)
  {
    ReleaseElFrame(m_elFrames.begin()->second);
    m_elFrames.erase(m_elFrames.begin());
  }
  m_elCond.notify_all();

  // no EL will come: shown before the first decodable EL picture (after a
  // start or seek), or the EL (in display order) is past this frame already
  auto never = [&] {
    return (m_noElBefore != INT64_MIN && pts + PTS_TOLERANCE_US < m_noElBefore) ||
           (!m_elFrames.empty() && m_elFrames.rbegin()->first > pts + PTS_TOLERANCE_US &&
            FindPts(m_elFrames, pts) == m_elFrames.end());
  };
  if (never())
  {
    m_noElSkipped++;
    return false;
  }
  const bool found = m_readyCond.wait_for(lock, std::chrono::milliseconds(EL_WAIT_MS), [&] {
    return m_stop || FindPts(m_elFrames, pts) != m_elFrames.end() || never();
  });
  if (found && !m_stop && FindPts(m_elFrames, pts) == m_elFrames.end())
  {
    m_noElSkipped++;
    return false;
  }
  if (!found || m_stop)
  {
    if (!m_stop && m_noPairLogged < 40)
    {
      m_noPairLogged++;
      auto el = m_elFrames.lower_bound(pts);
      CLog::Log(LOGINFO,
                "CAndroidDvFel: no EL for pts {} us: EL frames {} [{}..{}] next {}, EL queue {}"
                " (newest queued {}), decoder in {} out {} last out {}",
                pts, m_elFrames.size(), m_elFrames.empty() ? 0 : m_elFrames.begin()->first,
                m_elFrames.empty() ? 0 : m_elFrames.rbegin()->first,
                el == m_elFrames.end() ? -1 : el->first, m_elQueue.size(),
                m_elQueue.empty() ? -1 : m_elQueue.back().pts, m_elInputs.load(),
                m_elOutputs.load(), m_elLastOut.load());
    }
    return false;
  }

  // the frame goes to the caller; everything older is not needed any more
  auto f = FindPts(m_elFrames, pts);
  el = std::move(f->second);
  f->second.frame = nullptr;
  f->second.buf = -1;
  for (auto it = m_elFrames.begin(), end = std::next(f); it != end;)
  {
    ReleaseElFrame(it->second);
    it = m_elFrames.erase(it);
  }
  return el.frame || el.buf >= 0;
}

void CAndroidDvFel::GpuThread(std::promise<bool> ready)
{
  bool announced = false;
  auto fail = [&](const char* what) {
    CLog::Log(LOGERROR, "CAndroidDvFel: {}", what);
    if (!announced)
      ready.set_value(false);
    announced = true;
  };

  int dev = -1, ion = -1;
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLContext ctx = EGL_NO_CONTEXT;
  struct felgpu* fel = nullptr;
  dvfel_reg_bufs2 reg2{};
  dvfel_reg_bufs& reg = reg2.base;
  for (int i = 0; i < GPU_BUFFERS; i++)
    reg.in_fd[i] = reg.out_fd[i] = reg.el_fd[i] = -1;
  for (int i = 0; i < DVFEL_MAX_OUT; i++)
    reg2.lin_fd[i] = -1;
  GLuint tin[GPU_BUFFERS]{}, tout[GPU_BUFFERS]{}, fout[GPU_BUFFERS]{}, tel[EL_POOL]{};
  GLuint tlin[DVFEL_MAX_OUT]{}, flin[DVFEL_MAX_OUT]{};
  EGLImageKHR images[GPU_BUFFERS * 2 + EL_POOL + DVFEL_MAX_OUT]{};
  int nimg = 0;
  // the display shows the composed frames directly (module 2026.10.06.2+):
  // one display buffer per kernel slot, the job names the one to write; no
  // AFBC conversion (VICP) on the way to the display. Test: /sdcard/dvfel_afbc_out = 1
  // keeps the old way (out[buf], converted to AFBC by the kernel).
  const std::string modVersion = ReadLine("/sys/module/dvfel/version");
  const int slots = std::atoi(ReadLine("/sys/module/dvfel/parameters/slots").c_str());
  const bool linOut = modVersion >= std::string("2026.10.06.2") && slots >= 2 &&
                      slots <= DVFEL_MAX_OUT && ReadIntFile(AFBC_OUT_FILE, 0) != 1;
  const int nLin = linOut ? slots : 0;

  auto createImage = PFNEGLCREATEIMAGEKHRPROC(eglGetProcAddress("eglCreateImageKHR"));
  auto destroyImage = PFNEGLDESTROYIMAGEKHRPROC(eglGetProcAddress("eglDestroyImageKHR"));
  auto targetTexture =
      PFNGLEGLIMAGETARGETTEXTURE2DOESPROC(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  auto importTex = [&](int fd, int width, int height) -> GLuint {
    const EGLint attr[] = {EGL_WIDTH, width, EGL_HEIGHT, height,
                           EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
                           EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                           EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                           EGL_DMA_BUF_PLANE0_PITCH_EXT, width * 4,
                           EGL_NONE};
    EGLImageKHR img = createImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
    if (img == EGL_NO_IMAGE_KHR)
    {
      CLog::Log(LOGERROR, "CAndroidDvFel: eglCreateImageKHR(fd {}, {}x{}) failed: {:#x}", fd, width,
                height, eglGetError());
      return 0;
    }
    images[nimg++] = img;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    targetTexture(GL_TEXTURE_2D, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
      CLog::Log(LOGERROR, "CAndroidDvFel: binding the EGL image of fd {} failed: {:#x}", fd, err);
    return err == GL_NO_ERROR ? tex : 0;
  };

  do
  {
    if (!createImage || !destroyImage || !targetTexture)
    {
      fail("EGL image extensions missing");
      break;
    }
    // the process' default display (also Kodi's); it is never terminated here
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min))
    {
      fail("eglInitialize failed");
      break;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    // a high priority context (EGL_IMG_context_priority) made it worse: p95 38 -> 53 ms
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
    {
      fail("cannot create a surfaceless GLES 3.2 context");
      break;
    }

    fel = felgpu_create(m_width, m_height, EL_TAPS);
    if (!fel)
    {
      fail("felgpu_create failed");
      break;
    }

    dev = open(DVFEL_DEV, O_RDWR | O_CLOEXEC);
    ion = open(ION_DEV, O_RDONLY | O_CLOEXEC);
    const int heapId = ion >= 0 ? IonHeapId(ion) : -1;
    if (dev < 0 || heapId < 0)
    {
      fail("cannot open /dev/dvfel or find the codec_mm ION heap");
      break;
    }
    reg.width = m_width;
    reg.height = m_height;
    reg.count = GPU_BUFFERS;
    bool ok = true;
    // a frame buffer the GPU writes: texture and framebuffer of a new ION buffer
    auto outBuffer = [&](int& fd, GLuint& tex, GLuint& fbo) -> bool {
      fd = IonAlloc(ion, heapId, DVFEL_BUF_SIZE(m_width, m_height));
      tex = fd >= 0 ? importTex(fd, m_width, m_height) : 0;
      if (!tex)
        return false;
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
      return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    };
    for (int i = 0; i < GPU_BUFFERS && ok; i++)
    {
      reg.in_fd[i] = IonAlloc(ion, heapId, DVFEL_BUF_SIZE(m_width, m_height));
      tin[i] = reg.in_fd[i] >= 0 ? importTex(reg.in_fd[i], m_width, m_height) : 0;
      ok = tin[i] && (linOut || outBuffer(reg.out_fd[i], tout[i], fout[i]));
    }
    for (int i = 0; i < nLin && ok; i++)
      ok = outBuffer(reg2.lin_fd[i], tlin[i], flin[i]);
    if (!ok)
      CLog::Log(LOGERROR, "CAndroidDvFel: ION allocation of {} bytes from heap {} failed: {}",
                DVFEL_BUF_SIZE(m_width, m_height), heapId, strerror(errno));
    // hardware EL: the pool the kernel decompresses the EL pictures into
    for (int i = 0; i < EL_POOL && ok && m_hwEl; i++)
    {
      m_elPoolFd[i] = IonAlloc(ion, heapId, DVFEL_BUF_SIZE(m_elWidth, m_elHeight));
      tel[i] = m_elPoolFd[i] >= 0 ? importTex(m_elPoolFd[i], m_elWidth, m_elHeight) : 0;
      if (!tel[i])
      {
        CLog::Log(LOGERROR, "CAndroidDvFel: cannot set up the EL pool, software EL decoding");
        m_hwEl = false;
      }
    }
    if (!ok)
    {
      fail("cannot allocate or import the frame buffers");
      break;
    }
    glFinish();
    // routes the video composer through dvfel (kernel vc_path 2)
    reg2.out_count = nLin;
    if ((linOut ? ioctl(dev, DVFEL_IOC_REG_BUFS2, &reg2) : ioctl(dev, DVFEL_IOC_REG_BUFS, &reg)) < 0)
    {
      fail(linOut ? "DVFEL_IOC_REG_BUFS2 failed" : "DVFEL_IOC_REG_BUFS failed");
      break;
    }
    ready.set_value(true);
    announced = true;
    CLog::Log(LOGINFO, "CAndroidDvFel: GPU compositor ready ({}), {}",
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              linOut ? StringUtils::Format("{} display buffers (direct)", nLin)
                     : std::string("AFBC display"));

    std::shared_ptr<felgpu_meta> current;
    int dither = -1, pattern = -1, demo = -1;
    const bool noGpu = ReadIntFile("/sdcard/dvfel_nogpu", 0) == 1; // test: dvfel path without GPU work
    int64_t dumpUs = ReadIntFile(DUMP_FILE, -1);
    dumpUs = dumpUs >= 0 ? dumpUs * 1000 : -1;
    auto nextCheck = std::chrono::steady_clock::now();
    while (!m_stop)
    {
      if (std::chrono::steady_clock::now() >= nextCheck)
      {
        nextCheck += std::chrono::seconds(1);
        const int d = std::clamp(ReadIntFile(DITHER_FILE, 1), 0, 2);
        if (d != dither)
        {
          dither = d;
          felgpu_set_dither(fel, d);
          CLog::Log(LOGINFO, "CAndroidDvFel: dither mode {}", d);
        }
        const int p = ReadIntFile(PATTERN_FILE, 0) == 1 ? 1 : 0;
        if (p != pattern)
        {
          pattern = p;
          felgpu_set_pattern(fel, p);
        }
        const int dm = ReadIntFile(DEMO_FILE, 0) == 1 ? 1 : 0;
        if (dm != demo)
        {
          demo = dm;
          felgpu_set_demo(fel, dm);
          CLog::Log(LOGINFO, "CAndroidDvFel: demo mode {}", dm);
        }
      }

      dvfel_job2 job{};
      job.job.timeout_ms = 100;
      if (ioctl(dev, DVFEL_IOC_WAIT_JOB2, &job) < 0)
      {
        if (errno == ETIMEDOUT || errno == EINTR)
          continue;
        CLog::Log(LOGERROR, "CAndroidDvFel: DVFEL_IOC_WAIT_JOB2 failed: {}", strerror(errno));
        break;
      }

      dvfel_job_done done{};
      done.id = job.job.id;
      done.status = DVFEL_DONE_PASSTHROUGH;

      // where the composed frame goes
      GLuint target = 0;
      int targetFd = -1;
      if (linOut && job.out < static_cast<uint32_t>(nLin))
      {
        target = flin[job.out];
        targetFd = reg2.lin_fd[job.out];
      }
      else if (!linOut && job.job.buf < GPU_BUFFERS)
      {
        target = fout[job.job.buf];
        targetFd = reg.out_fd[job.job.buf];
      }

      ElFrame el;
      bool haveEl = false;
      int64_t pts = -1;
      if (job.job.buf < GPU_BUFFERS && target && !noGpu)
      {
        pts = (job.job.flags & DVFEL_JOB_RPU_CRC) ? PtsForRpuCrc(job.rpu_crc) : -1;
        if (pts >= 0)
          haveEl = WaitForFrame(pts, el);
        else if (m_noCrcLogged < 10)
        {
          m_noCrcLogged++;
          std::lock_guard<std::mutex> lock(m_lock);
          std::string pending;
          for (auto it = m_crcByPts.begin(); it != m_crcByPts.end() && pending.size() < 80; ++it)
            pending += StringUtils::Format(" {}:{:08x}", it->first / 1000, it->second);
          CLog::Log(LOGINFO, "CAndroidDvFel: frame {} (rpu crc {:08x}, flags {:#x}) matches no packet "
                             "(pending {}:{})",
                    job.seq, job.rpu_crc, job.job.flags, m_crcByPts.size(), pending);
        }
      }

      if (haveEl)
      {
        const auto t0 = std::chrono::steady_clock::now();
        if (el.meta != current)
        {
          felgpu_set_meta(fel, el.meta.get());
          current = el.meta;
        }
        if (el.buf >= 0)
        {
          felgpu_set_el_tex(fel, tel[el.buf]);
        }
        else
        {
          const AVFrame* f = el.frame;
          felgpu_set_el(fel, reinterpret_cast<const uint16_t*>(f->data[0]), f->linesize[0] / 2,
                        reinterpret_cast<const uint16_t*>(f->data[1]),
                        reinterpret_cast<const uint16_t*>(f->data[2]), f->linesize[1] / 2);
        }
        felgpu_run(fel, tin[job.job.buf], target);
        glFinish();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        m_gpuMsSum += ms;
        if (m_gpuMs.size() < 1000000)
          m_gpuMs.push_back(static_cast<float>(ms));
        done.status = DVFEL_DONE_COMPOSED;
        m_composed++;
        if (dumpUs >= 0 && pts >= dumpUs)
        {
          dumpUs = -1;
          const size_t size = static_cast<size_t>(m_width) * m_height * 4;
          for (const char* what : {"in", "out"})
          {
            const int fd = what[0] == 'i' ? reg.in_fd[job.job.buf] : targetFd;
            void* p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
            const std::string name = StringUtils::Format("/sdcard/dvfel_{}_{}_{}.raw", what,
                                                         el.buf >= 0 ? "hw" : "sw", pts);
            FILE* f = p != MAP_FAILED ? fopen(name.c_str(), "wb") : nullptr;
            if (f)
            {
              fwrite(p, 1, size, f);
              fclose(f);
              CLog::Log(LOGINFO, "CAndroidDvFel: frame {} us written to {}", pts, name);
            }
            if (p != MAP_FAILED)
              munmap(p, size);
          }
        }
      }
      else
      {
        // no EL (e.g. the leading pictures after a seek): the frame's own
        // mapping without the residual looks close to its neighbours, the
        // base layer as is would flash
        std::shared_ptr<felgpu_meta> meta;
        if (pts >= 0)
        {
          std::lock_guard<std::mutex> lock(m_lock);
          auto m = FindPts(m_metaByPts, pts);
          if (m != m_metaByPts.end())
            meta = m->second;
        }
        if (meta)
        {
          felgpu_meta mapping = *meta;
          mapping.use_el = 0;
          felgpu_set_meta(fel, &mapping);
          current = nullptr;
          felgpu_run(fel, tin[job.job.buf], target);
          glFinish();
          done.status = DVFEL_DONE_COMPOSED;
          m_mappedOnly++;
        }
        m_passthrough++;
      }
      {
        std::lock_guard<std::mutex> lock(m_lock);
        ReleaseElFrame(el);
      }
      ioctl(dev, DVFEL_IOC_JOB_DONE, &done);
    }
  } while (false);

  if (dev >= 0)
  {
    ioctl(dev, DVFEL_IOC_UNREG_BUFS);
    close(dev);
  }
  if (fel)
    felgpu_destroy(fel);
  if (ctx != EGL_NO_CONTEXT)
  {
    glDeleteFramebuffers(GPU_BUFFERS, fout);
    glDeleteFramebuffers(DVFEL_MAX_OUT, flin);
    glDeleteTextures(GPU_BUFFERS, tin);
    glDeleteTextures(GPU_BUFFERS, tout);
    glDeleteTextures(DVFEL_MAX_OUT, tlin);
    glDeleteTextures(EL_POOL, tel);
    for (int i = 0; i < nimg; i++)
      destroyImage(dpy, images[i]);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
  }
  for (int i = 0; i < GPU_BUFFERS; i++)
  {
    if (reg.in_fd[i] >= 0)
      close(reg.in_fd[i]);
    if (reg.out_fd[i] >= 0)
      close(reg.out_fd[i]);
  }
  for (int i = 0; i < DVFEL_MAX_OUT; i++)
    if (reg2.lin_fd[i] >= 0)
      close(reg2.lin_fd[i]);
  if (ion >= 0)
    close(ion);
  if (!announced)
    ready.set_value(false);
}
