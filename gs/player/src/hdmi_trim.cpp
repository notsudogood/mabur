#include "hdmi_trim.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace maburplay {
namespace hpll {

Setting decode(uint32_t con0, uint32_t con1, uint32_t con2) {
  Setting s;
  s.fbdiv = con0 & 0xfffu;
  s.postdiv1 = (con0 >> 12) & 0x7u;
  s.integer = ((con1 >> 12) & 0x1u) != 0;
  s.postdiv2 = (con1 >> 6) & 0x7u;
  s.refdiv = con1 & 0x3fu;
  s.frac = con2 & 0xffffffu;
  return s;
}

double rate_hz(const Setting& s) {
  const double div = static_cast<double>(s.refdiv) * s.postdiv1 * s.postdiv2;
  if (div <= 0.0) return 0.0;
  const double n = static_cast<double>(s.fbdiv) +
                   (s.integer ? 0.0 : static_cast<double>(s.frac) / kFracOne);
  return kRefHz * n / div;
}

bool same_point(const Setting& a, const Setting& b) {
  return a.fbdiv == b.fbdiv && a.postdiv1 == b.postdiv1 && a.postdiv2 == b.postdiv2 &&
         a.refdiv == b.refdiv && a.integer == b.integer;
}

uint32_t con0_write(uint32_t postdiv1, uint32_t fbdiv) {
  return (0x7fffu << 16) | ((postdiv1 & 0x7u) << 12) | (fbdiv & 0xfffu);
}

uint32_t con1_write(bool integer, uint32_t postdiv2, uint32_t refdiv) {
  // Mask 0x11ff: dsmpd, postdiv2 and refdiv. The spike's first mask missed
  // the refdiv bits and left the pixel clock at a quarter.
  return (0x11ffu << 16) | ((integer ? 1u : 0u) << 12) | ((postdiv2 & 0x7u) << 6) |
         (refdiv & 0x3fu);
}

uint32_t con2_value(uint32_t old_con2, uint32_t frac) {
  return (old_con2 & 0xff000000u) | (frac & 0xffffffu);
}

namespace {
constexpr double kNominalN = 74.25;   // every trim point: fbdiv 74 + 0.25
constexpr uint32_t kEdge = kFracOne / 64;
}  // namespace

bool frac_for_ppm(double ppm, uint32_t* frac) {
  const double n = kNominalN * (1.0 + ppm * 1e-6);
  const double f = std::round((n - static_cast<double>(kPointFbdiv)) * kFracOne);
  if (!(f >= kEdge && f <= kFracOne - kEdge)) return false;
  *frac = static_cast<uint32_t>(f);
  return true;
}

double ppm_for_frac(uint32_t frac) {
  const double n = static_cast<double>(kPointFbdiv) + static_cast<double>(frac) / kFracOne;
  return (n / kNominalN - 1.0) * 1e6;
}

}  // namespace hpll

namespace {

class DevMemIo : public RegIo {
 public:
  explicit DevMemIo(void* map) : map_(map), base_(static_cast<volatile uint32_t*>(map)) {}
  ~DevMemIo() override { ::munmap(map_, kPage); }
  uint32_t read(uint32_t off) override { return base_[off / 4]; }
  void write(uint32_t off, uint32_t value) override { base_[off / 4] = value; }
  static constexpr size_t kPage = 4096;

 private:
  void* map_;
  volatile uint32_t* base_;
};

std::string describe(const hpll::Setting& s) {
  char b[160];
  std::snprintf(b, sizeof(b), "fbdiv %u%s, postdivs %ux%u, refdiv %u = %.3f MHz", s.fbdiv,
                s.integer ? " (integer)" : " + frac", s.postdiv1, s.postdiv2, s.refdiv,
                hpll::rate_hz(s) / 1e6);
  return b;
}

}  // namespace

std::unique_ptr<RegIo> open_pmucru(std::string* why) {
  const int fd = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
  if (fd < 0) {
    *why = std::string("open /dev/mem: ") + std::strerror(errno);
    return nullptr;
  }
  void* m = ::mmap(nullptr, DevMemIo::kPage, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   static_cast<off_t>(hpll::kPmucruBase));
  const int err = errno;
  ::close(fd);
  if (m == MAP_FAILED) {
    *why = std::string("map the PMUCRU page: ") + std::strerror(err);
    return nullptr;
  }
  return std::make_unique<DevMemIo>(m);
}

bool soc_is_rk3566(std::string* why) {
  std::ifstream f("/proc/device-tree/compatible", std::ios::binary);
  std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (c.find("rockchip,rk3566") != std::string::npos) return true;
  std::replace(c.begin(), c.end(), '\0', ' ');
  *why = c.empty() ? std::string("no /proc/device-tree/compatible")
                   : "not an RK3566 (compatible: " + c + ")";
  return false;
}

HdmiTrim::~HdmiTrim() {
  if (engaged_) set_ppm(0.0);
}

bool HdmiTrim::wait_lock() {
  for (int i = 0; i < 20; ++i) {
    if (io_->read(hpll::kCon1) & hpll::kLockBit) return true;
    ::usleep(1000);
  }
  return false;
}

bool HdmiTrim::engage(int clock_khz, std::string* why, bool* moved) {
  using namespace hpll;
  *moved = false;
  engaged_ = false;
  half_rate_ = false;
  if (clock_khz != kClock148Khz) {
    char b[120];
    std::snprintf(b, sizeof(b),
                  "pixel clock %d kHz: only 148500 kHz (1920x1080@60) has a trim point",
                  clock_khz);
    *why = b;
    return false;
  }
  const Setting now = decode(io_->read(kCon0), io_->read(kCon1), io_->read(kCon2));
  // Full rate or half rate, by what the PLL reads: its point or its
  // kernel setting. The four settings are pairwise distinct.
  const bool half = same_point(now, point_74()) || same_point(now, kernel_74());
  const Setting point = half ? point_74() : point_148();
  const Setting kern = half ? kernel_74() : kernel_148();
  if (!same_point(now, point)) {
    if (!same_point(now, kern)) {
      *why = "HPLL reads " + describe(now) +
             ", neither of the kernel's 1080p60 settings (148.5 or 74.25 MHz); left alone";
      return false;
    }
    io_->write(kCon2, con2_value(io_->read(kCon2), point.frac));
    io_->write(kCon0, con0_write(point.postdiv1, point.fbdiv));
    io_->write(kCon1, con1_write(point.integer, point.postdiv2, point.refdiv));
    writes_ += 3;
    *moved = true;
    const bool locked = wait_lock();
    const Setting after = decode(io_->read(kCon0), io_->read(kCon1), io_->read(kCon2));
    if (!locked || !same_point(after, point) || after.frac != point.frac) {
      io_->write(kCon2, con2_value(io_->read(kCon2), 0));
      io_->write(kCon0, con0_write(kern.postdiv1, kern.fbdiv));
      io_->write(kCon1, con1_write(kern.integer, kern.postdiv2, kern.refdiv));
      writes_ += 3;
      *why = std::string("the PLL did not take the trim point (") +
             (locked ? "" : "no lock, ") + "reads " + describe(after) +
             "); the kernel's setting is back";
      return false;
    }
  }
  frac_ = decode(io_->read(kCon0), io_->read(kCon1), io_->read(kCon2)).frac;
  applied_ppm_ = ppm_for_frac(frac_);
  engaged_ = true;
  half_rate_ = half;
  set_ppm(0.0);
  return true;
}

double HdmiTrim::set_ppm(double ppm) {
  if (!engaged_) return 0.0;
  const double lim = static_cast<double>(max_ppm_);
  saturated_ = std::fabs(ppm) > lim;
  uint32_t frac = 0;
  if (!hpll::frac_for_ppm(std::clamp(ppm, -lim, lim), &frac)) return applied_ppm_;
  if (frac != frac_) {
    io_->write(hpll::kCon2, hpll::con2_value(io_->read(hpll::kCon2), frac));
    ++writes_;
    frac_ = frac;
  }
  applied_ppm_ = hpll::ppm_for_frac(frac_);
  return applied_ppm_;
}

}  // namespace maburplay
