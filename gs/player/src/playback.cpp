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
    // The DvrNamer patterns: record-NNNN.mp4 side by side (before gs18, or
    // dvr.folders = false), or a record-NNNN/ folder holding
    // record-NNNN.mp4. Legacy record_<date>.mp4 and anything else skipped.
    if (fn.size() < 8 || fn.compare(0, 7, "record-") != 0) continue;
    std::error_code fe;
    const bool is_dir = e.is_directory(fe);
    std::string stem = fn;
    if (!is_dir) {
      if (fn.size() < 12 || fn.compare(fn.size() - 4, 4, ".mp4") != 0) continue;
      stem = fn.substr(0, fn.size() - 4);
    }
    const std::string num = stem.substr(7);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) continue;
    Recording r;
    r.path = is_dir ? (e.path() / (stem + ".mp4")).string() : e.path().string();
    r.name = stem;
    r.index = std::atoi(num.c_str());
    r.bytes = static_cast<uint64_t>(std::filesystem::file_size(r.path, fe));
    if (fe) continue;   // a folder without its video
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
