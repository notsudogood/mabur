#include "playback.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "mabur/dvr_demux.h"

namespace maburplay {

std::vector<Recording> list_recordings(const std::string& dir, size_t max) {
  std::vector<Recording> all;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    const std::string fn = e.path().filename().string();
    // record-NNNN.mp4: the DvrNamer pattern; legacy record_<date>.mp4 skipped.
    if (fn.size() < 15 || fn.compare(0, 7, "record-") != 0 ||
        fn.compare(fn.size() - 4, 4, ".mp4") != 0)
      continue;
    const std::string num = fn.substr(7, fn.size() - 11);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) continue;
    Recording r;
    r.path = e.path().string();
    r.name = fn.substr(0, fn.size() - 4);
    r.index = std::atoi(num.c_str());
    r.bytes = static_cast<uint64_t>(e.file_size(ec));
    all.push_back(r);
  }
  std::sort(all.begin(), all.end(),
            [](const Recording& a, const Recording& b) { return a.index > b.index; });
  std::vector<Recording> out;
  for (Recording& r : all) {
    if (out.size() >= max) break;
    const auto d = mabur::DvrDemux::probe_duration_us(r.path);
    if (!d || *d == 0) continue;   // unreadable or empty: not offered
    r.duration_us = *d;
    out.push_back(r);
  }
  return out;
}

}  // namespace maburplay
