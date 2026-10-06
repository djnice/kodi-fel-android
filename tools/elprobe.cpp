// elprobe: decode a raw HEVC elementary stream (a Dolby Vision EL) with the
// Amlogic hardware decoder through MediaCodec into an AImageReader surface and
// report the decoder frame (vframe) behind every output buffer (dvfel
// DVFEL_IOC_EL_PROBE on the buffer's dma-buf).
//
// usage: elprobe <file.hevc> [frames] [usage hex] [images] [codec]
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../kernel/dvfel_uapi.h"

namespace
{
struct NativeHandle
{
  int version, numFds, numInts;
  int data[0];
};
using GetNativeHandle = const NativeHandle* (*)(const AHardwareBuffer*);

int64_t NowUs()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// Annex-B stream -> access units
std::vector<std::vector<uint8_t>> SplitAus(const std::vector<uint8_t>& s)
{
  std::vector<size_t> starts; // NAL payload offsets
  for (size_t i = 0; i + 3 < s.size(); i++)
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
    {
      starts.push_back(i + 3);
      i += 2;
    }
  std::vector<std::vector<uint8_t>> aus;
  std::vector<uint8_t> cur;
  bool vcl = false;
  for (size_t n = 0; n < starts.size(); n++)
  {
    const size_t b = starts[n];
    size_t e = n + 1 < starts.size() ? starts[n + 1] - 3 : s.size();
    while (e > b && s[e - 1] == 0)
      e--;
    const int type = (s[b] >> 1) & 0x3f;
    const bool isVcl = type < 32;
    const bool first = isVcl && b + 2 < s.size() && (s[b + 2] & 0x80);
    const bool startsAu =
        (vcl && (first || type == 35 || (type >= 32 && type <= 34) || type == 39));
    if (startsAu && !cur.empty())
    {
      aus.push_back(std::move(cur));
      cur.clear();
      vcl = false;
    }
    static const uint8_t sc[] = {0, 0, 0, 1};
    cur.insert(cur.end(), sc, sc + 4);
    cur.insert(cur.end(), s.begin() + b, s.begin() + e);
    vcl |= isVcl;
  }
  if (!cur.empty())
    aus.push_back(std::move(cur));
  return aus;
}
// ION, see AndroidDvFel.cpp
struct ion_allocation_data
{
  uint64_t len;
  uint32_t heap_id_mask, flags, fd, unused;
};
struct ion_heap_data
{
  char name[32];
  uint32_t type, heap_id, reserved0, reserved1, reserved2;
};
struct ion_heap_query
{
  uint32_t cnt, reserved0;
  uint64_t heaps;
  uint32_t reserved1, reserved2;
};
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_HEAP_QUERY _IOWR('I', 8, struct ion_heap_query)

int IonAllocCma(size_t len)
{
  const int ion = open("/dev/ion", O_RDONLY);
  if (ion < 0)
    return -1;
  ion_heap_query q{};
  ioctl(ion, ION_IOC_HEAP_QUERY, &q);
  std::vector<ion_heap_data> heaps(q.cnt);
  q.heaps = reinterpret_cast<uintptr_t>(heaps.data());
  ioctl(ion, ION_IOC_HEAP_QUERY, &q);
  int fd = -1;
  for (auto& h : heaps)
    if (!strcmp(h.name, "codec_mm_cma"))
    {
      ion_allocation_data a{};
      a.len = len;
      a.heap_id_mask = 1u << h.heap_id;
      a.flags = 1u << 30;
      if (ioctl(ion, ION_IOC_ALLOC, &a) == 0)
        fd = a.fd;
    }
  close(ion);
  return fd;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: %s <file.hevc> [frames] [usage hex] [images] [codec]\n", argv[0]);
    return 1;
  }
  const int maxFrames = argc > 2 ? atoi(argv[2]) : 100;
  const uint64_t usage = argc > 3 ? strtoull(argv[3], nullptr, 16)
                                  : AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
  const int maxImages = argc > 4 ? atoi(argv[4]) : 2;
  const char* codecName = argc > 5 ? argv[5] : "OMX.amlogic.hevc.decoder.awesome2";

  FILE* f = fopen(argv[1], "rb");
  if (!f)
  {
    perror(argv[1]);
    return 1;
  }
  std::vector<uint8_t> stream;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    stream.insert(stream.end(), buf, buf + n);
  fclose(f);
  // a Kodi EL dump (*.bin, /sdcard/dvfel_eldump): [u32 size][i64 pts][data] records,
  // fed with their pts unless SEQPTS=1
  std::vector<std::vector<uint8_t>> aus;
  std::vector<int64_t> ptsOf;
  const size_t len = strlen(argv[1]);
  if (len > 4 && !strcmp(argv[1] + len - 4, ".bin"))
  {
    for (size_t off = 0; off + 12 <= stream.size();)
    {
      uint32_t sz;
      int64_t t;
      memcpy(&sz, &stream[off], 4);
      memcpy(&t, &stream[off + 4], 8);
      off += 12;
      if (off + sz > stream.size())
        break;
      aus.emplace_back(stream.begin() + off, stream.begin() + off + sz);
      ptsOf.push_back(t);
      off += sz;
    }
    if (getenv("SEQPTS") && atoi(getenv("SEQPTS")))
      ptsOf.clear();
  }
  else
    aus = SplitAus(stream);
  printf("%zu access units\n", aus.size());

  // binder callbacks (not in the public NDK headers)
  if (auto start = reinterpret_cast<void (*)()>(
          dlsym(dlopen("libbinder_ndk.so", RTLD_NOW), "ABinderProcess_startThreadPool")))
    start();

  auto getHandle = reinterpret_cast<GetNativeHandle>(
      dlsym(dlopen("libnativewindow.so", RTLD_NOW), "AHardwareBuffer_getNativeHandle"));
  const int dev = open("/dev/dvfel", O_RDWR);
  printf("getNativeHandle %p, /dev/dvfel %d, usage %#llx\n", getHandle, dev,
         (unsigned long long)usage);

  // IMPORT=1: DVFEL_IOC_EL_IMPORT of every picture; DUMP=n: write output n
  const bool doImport = getenv("IMPORT") && atoi(getenv("IMPORT"));
  const int dumpAt = getenv("DUMP") ? atoi(getenv("DUMP")) : -1;
  const int W = getenv("W") ? atoi(getenv("W")) : 1920, H = getenv("H") ? atoi(getenv("H")) : 1080;
  const size_t linSize = DVFEL_BUF_SIZE(1920, 1080);
  const int linFd = doImport ? IonAllocCma(linSize) : -1;
  if (doImport)
    printf("import buffer fd %d (%zu bytes)\n", linFd, linSize);
  int imported = 0, importErr = 0;
  uint64_t importUs = 0, importMax = 0;

  AImageReader* reader = nullptr;
  media_status_t st =
      AImageReader_newWithUsage(W, H, AIMAGE_FORMAT_PRIVATE, usage, maxImages, &reader);
  if (st != AMEDIA_OK)
  {
    printf("AImageReader_newWithUsage: %d\n", st);
    return 1;
  }
  ANativeWindow* win = nullptr;
  AImageReader_getWindow(reader, &win);

  AMediaCodec* codec = AMediaCodec_createCodecByName(codecName);
  if (!codec)
  {
    printf("cannot create %s\n", codecName);
    return 1;
  }
  AMediaFormat* fmt = AMediaFormat_new();
  AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, W);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, H);
  st = AMediaCodec_configure(codec, fmt, win, nullptr, 0);
  printf("configure: %d\n", st);
  st = AMediaCodec_start(codec);
  printf("start: %d\n", st);

  size_t in = 0;
  int out = 0, images = 0, tenBit = 0, compressed = 0;
  bool eosSent = false;
  const int64_t t0 = NowUs();
  int64_t lastProgress = t0;
  // FPS=n: feed in real time, LEAD pictures ahead of the clock (like Kodi's pacing)
  const double fps = getenv("FPS") ? atof(getenv("FPS")) : 0;
  const int lead = getenv("LEAD") ? atoi(getenv("LEAD")) : 8;
  while (out < maxFrames && NowUs() - lastProgress < 3000000)
  {
    const bool due = fps <= 0 || (int)in < lead || NowUs() - t0 >= (int64_t)((in - lead) * 1e6 / fps);
    if (!eosSent && due)
    {
      ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 2000);
      if (idx >= 0)
      {
        size_t cap;
        uint8_t* p = AMediaCodec_getInputBuffer(codec, idx, &cap);
        if (in < aus.size() && (int)in < maxFrames + 16 && aus[in].size() <= cap)
        {
          memcpy(p, aus[in].data(), aus[in].size());
          AMediaCodec_queueInputBuffer(codec, idx, 0, aus[in].size(), in < ptsOf.size() ? ptsOf[in] : (int64_t)in * 41708, 0);
          in++;
        }
        else
        {
          AMediaCodec_queueInputBuffer(codec, idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          eosSent = true;
        }
      }
    }
    AMediaCodecBufferInfo info;
    ssize_t o = AMediaCodec_dequeueOutputBuffer(codec, &info, 2000);
    if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
    {
      AMediaFormat* of = AMediaCodec_getOutputFormat(codec);
      printf("output format: %s\n", AMediaFormat_toString(of));
      AMediaFormat_delete(of);
      continue;
    }
    if (o < 0)
      continue;
    lastProgress = NowUs();
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
    {
      AMediaCodec_releaseOutputBuffer(codec, o, false);
      break;
    }
    AMediaCodec_releaseOutputBuffer(codec, o, true);
    out++;

    AImage* img = nullptr;
    for (int tries = 0; tries < 50 && !img; tries++)
    {
      if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK)
      {
        img = nullptr;
        usleep(1000);
      }
    }
    if (!img)
    {
      printf("out %d pts %lld: no image\n", out, (long long)info.presentationTimeUs);
      continue;
    }
    images++;
    int64_t ts = 0;
    AImage_getTimestamp(img, &ts);
    AHardwareBuffer* hb = nullptr;
    AImage_getHardwareBuffer(img, &hb);
    AHardwareBuffer_Desc desc{};
    if (hb)
      AHardwareBuffer_describe(hb, &desc);
    const NativeHandle* h = hb && getHandle ? getHandle(hb) : nullptr;
    char line[512];
    int len = snprintf(line, sizeof(line), "out %3d pts %8lld img ts %8lld us buf %ux%u fmt %#x usage %#llx",
                       out, (long long)info.presentationTimeUs, (long long)(ts / 1000), desc.width,
                       desc.height, desc.format, (unsigned long long)desc.usage);
    if (h)
    {
      len += snprintf(line + len, sizeof(line) - len, " fds %d ints %d", h->numFds, h->numInts);
      for (int i = 0; i < h->numFds && dev >= 0; i++)
      {
        dvfel_el_probe pr{};
        pr.fd = h->data[i];
        int r = ioctl(dev, DVFEL_IOC_EL_PROBE, &pr);
        if (r < 0)
          len += snprintf(line + len, sizeof(line) - len, " [fd%d: err %d]", i, -errno);
        else
        {
          len += snprintf(line + len, sizeof(line) - len,
                          " [fd%d: type %#x bd %#x flag %#x %ux%u comp %ux%u idx %u planes %u pts %llu "
                          "head %#llx]",
                          i, pr.type, pr.bitdepth, pr.flag, pr.width, pr.height, pr.comp_width,
                          pr.comp_height, pr.index, pr.plane_num, (unsigned long long)pr.pts_us,
                          (unsigned long long)pr.head_addr);
          if (i == 0 && pr.type)
          {
            if ((pr.bitdepth & (3 << 8)) == (2 << 8)) // BITDEPTH_Y10
              tenBit++;
            if (pr.type & 0x100000) // VIDTYPE_COMPRESS
              compressed++;
          }
        }
      }
    }
    if (out <= 12 || out % 50 == 0)
      printf("%s\n", line);
    if (h && linFd >= 0 && dev >= 0)
    {
      dvfel_el_import im{};
      im.src_fd = h->data[0];
      im.dst_fd = linFd;
      im.width = 1920;
      im.height = 1080;
      if (ioctl(dev, DVFEL_IOC_EL_IMPORT, &im) < 0)
      {
        if (importErr++ < 5)
          printf("out %d: EL_IMPORT failed: %s\n", out, strerror(errno));
      }
      else
      {
        imported++;
        importUs += im.us;
        importMax = im.us > importMax ? im.us : importMax;
        if (out - 1 == dumpAt)
        {
          void* p = mmap(nullptr, linSize, PROT_READ, MAP_SHARED, linFd, 0);
          char name[64];
          snprintf(name, sizeof(name), "/data/local/tmp/el_%d.raw", dumpAt);
          FILE* df = p != MAP_FAILED ? fopen(name, "wb") : nullptr;
          if (df)
          {
            fwrite(p, 1, 1920 * 1080 * 4, df);
            fclose(df);
            printf("dumped output %d (pts %lld) to %s\n", dumpAt, (long long)info.presentationTimeUs,
                   name);
          }
          if (p != MAP_FAILED)
            munmap(p, linSize);
        }
      }
    }
    AImage_delete(img);
  }
  const double secs = (NowUs() - t0) / 1e6;
  printf("in %zu out %d images %d (%.1f fps), compressed %d, y10 %d\n", in, out, images,
         out / secs, compressed, tenBit);
  if (doImport)
    printf("imported %d (errors %d), VICP avg %.1f ms max %.1f ms\n", imported, importErr,
           imported ? importUs / 1000.0 / imported : 0.0, importMax / 1000.0);
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  AImageReader_delete(reader);
  return 0;
}
