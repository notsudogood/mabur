#include "mabur/dvr_name.h"

#include <algorithm>
#include <cstdio>

#include <cerrno>

#include <dirent.h>
#include <sys/stat.h>

namespace mabur {

std::string DvrNamer::next(const std::string& dir, bool folder) {
  // Best-effort scan of a directory maburplay does not own exclusively
  // (the card also holds flight-NNNN.jsonl, ctl-NNNN logs and recordings
  // from older naming). Malformed and foreign entries are ignored, and an
  // unreadable dir is not fatal: the fopen that follows reports it, and
  // refusing to name would turn a missing SD card into a silent path.
  int idx = last_issued_ + 1;
  if (DIR* d = opendir(dir.c_str())) {
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      int n = -1;
      if (std::sscanf(ent->d_name, "record-%d", &n) == 1 && n >= 0)
        idx = std::max(idx, n + 1);
    }
    closedir(d);
  }
  last_issued_ = idx;

  // %04d is a minimum width, not a cap: past 9999 the name grows a digit
  // rather than wrapping onto a file that already exists.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "record-%04d", idx);
  const std::string base(buf);
  return folder ? dir + "/" + base + "/" + base + ".mp4" : dir + "/" + base + ".mp4";
}

bool make_recording_dir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string folder = path.substr(0, slash);
  const std::string file = path.substr(slash + 1);
  const size_t fslash = folder.find_last_of('/');
  const std::string folder_name = fslash == std::string::npos ? folder : folder.substr(fslash + 1);
  const size_t dot = file.find_last_of('.');
  const std::string stem = dot == std::string::npos ? file : file.substr(0, dot);
  if (folder_name.empty() || folder_name != stem) return true;   // not the recording's own
  if (::mkdir(folder.c_str(), 0755) == 0 || errno == EEXIST) return true;
  return false;
}

}  // namespace mabur
