#include "mabur/cpu_stat.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mabur {

namespace {

// Parses the counters after "cpu" / "cpuN" on one /proc/stat line into
// busy and total. False when fewer than four counters (user nice system idle)
// are present.
bool parse_counters(const char* p, uint64_t& busy, uint64_t& total) {
  // user nice system idle iowait irq softirq steal
  uint64_t c[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int n = 0;
  while (n < 8) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!std::isdigit(static_cast<unsigned char>(*p))) break;
    char* end = nullptr;
    c[n++] = std::strtoull(p, &end, 10);
    p = end;
  }
  if (n < 4) return false;
  total = 0;
  for (int i = 0; i < 8; ++i) total += c[i];
  busy = total - (c[3] + c[4]);
  return true;
}

std::optional<double> delta_pct(bool had, uint64_t busy0, uint64_t total0, uint64_t busy,
                                uint64_t total) {
  if (!had || total <= total0 || busy < busy0) return std::nullopt;
  return 100.0 * static_cast<double>(busy - busy0) / static_cast<double>(total - total0);
}

}  // namespace

CpuSample CpuStatSampler::sample(const char* path) {
  CpuSample out;
  FILE* f = std::fopen(path, "r");
  if (!f) {
    all_.have = false;
    cores_.clear();
    return out;
  }
  bool got_all = false;
  std::vector<bool> seen;
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "cpu", 3) != 0) break;  // the cpu lines lead the file
    uint64_t busy = 0, total = 0;
    if (line[3] == ' ' || line[3] == '\t') {
      if (!parse_counters(line + 3, busy, total)) continue;
      out.total_pct = delta_pct(all_.have, all_.busy, all_.total, busy, total);
      all_ = {true, busy, total};
      got_all = true;
    } else if (std::isdigit(static_cast<unsigned char>(line[3]))) {
      char* end = nullptr;
      const unsigned long idx = std::strtoul(line + 3, &end, 10);
      if (idx >= 64 || !parse_counters(end, busy, total)) continue;  // sanity bound
      if (cores_.size() <= idx) cores_.resize(idx + 1);
      if (out.core_pct.size() <= idx) out.core_pct.resize(idx + 1);
      if (seen.size() <= idx) seen.resize(idx + 1, false);
      Acc& a = cores_[idx];
      out.core_pct[idx] = delta_pct(a.have, a.busy, a.total, busy, total);
      a = {true, busy, total};
      seen[idx] = true;
    }
  }
  std::fclose(f);
  if (!got_all) {
    all_.have = false;
    cores_.clear();
    return CpuSample{};
  }
  // A core missing from this read went offline: drop its baseline so its
  // next appearance starts fresh instead of spanning the gap.
  for (size_t i = 0; i < cores_.size(); ++i)
    if (i >= seen.size() || !seen[i]) cores_[i].have = false;
  return out;
}

}  // namespace mabur
