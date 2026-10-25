#include "camera_ctl.h"

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace mabur {

namespace {

bool read_file(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

// Writes `text` to `path` and fsyncs it, so a power cut right after cannot
// leave half a file where the next boot reads it.
bool write_file(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() &&
                  std::fflush(f) == 0 && ::fsync(fileno(f)) == 0;
  return std::fclose(f) == 0 && ok;
}

}  // namespace

std::optional<CamPending> cam_take_pending(const CamFiles& f) {
  std::string text;
  if (!read_file(f.pending, &text)) return std::nullopt;
  auto p = parse_cam_pending(text);
  if (!p) {
    std::fprintf(stderr, "camera: unreadable %s removed\n", f.pending.c_str());
    std::remove(f.pending.c_str());
  }
  return p;
}

uint8_t cam_take_result(const CamFiles& f) {
  const uint8_t idle = cam::pack_state(cam::State::Idle, cam::kNoPreset);
  std::string text;
  if (!read_file(f.result, &text)) return idle;
  std::remove(f.result.c_str());
  int id = -1;
  if (std::sscanf(text.c_str(), "rolled_back %d", &id) != 1 || !cam::preset(static_cast<uint8_t>(id)))
    return idle;
  return cam::pack_state(cam::State::RolledBack, static_cast<uint8_t>(id));
}

cam::State cam_apply(const CamFiles& f, const Config& cur, const cam::Preset& p,
                     bool driver_switch, std::string* err) {
  std::string text;
  if (!read_file(f.conf, &text)) {
    *err = "cannot read " + f.conf;
    return cam::State::BadConfig;
  }
  const std::string next = preset_config_text(text, cur, p);
  if (!write_file(f.next, next)) {
    *err = "cannot write " + f.next;
    return cam::State::BadConfig;
  }
  try {
    load_config(f.next, nullptr);
  } catch (const std::exception& e) {
    *err = e.what();
    std::remove(f.next.c_str());
    return cam::State::BadConfig;
  }
  if (!write_file(f.prev, text)) {
    *err = "cannot write " + f.prev;
    std::remove(f.next.c_str());
    return cam::State::BadConfig;
  }
  const CamPending marker{p.id, driver_switch};
  if (!write_file(f.pending, format_cam_pending(marker))) {
    *err = "cannot write " + f.pending;
    std::remove(f.next.c_str());
    return cam::State::BadConfig;
  }
  if (driver_switch && !write_file(f.star6e_flag, "camera tab\n")) {
    *err = "cannot write " + f.star6e_flag;
    std::remove(f.pending.c_str());
    std::remove(f.next.c_str());
    return cam::State::BadConfig;
  }
  // The one step that changes what boots: an atomic rename.
  if (std::rename(f.next.c_str(), f.conf.c_str()) != 0) {
    *err = "cannot replace " + f.conf;
    std::remove(f.pending.c_str());
    if (driver_switch) std::remove(f.star6e_flag.c_str());
    std::remove(f.next.c_str());
    return cam::State::BadConfig;
  }
  ::sync();
  return cam::State::Applying;
}

void cam_commit(const CamFiles& f) {
  std::remove(f.pending.c_str());
  ::sync();
}

void cam_roll_back(const CamFiles& f, const CamPending& p) {
  if (std::rename(f.prev.c_str(), f.conf.c_str()) != 0)
    std::fprintf(stderr, "camera: rollback could not restore %s from %s\n", f.conf.c_str(),
                 f.prev.c_str());
  if (p.driver_switched) std::remove(f.star6e_flag.c_str());
  std::remove(f.pending.c_str());
  write_file(f.result, "rolled_back " + std::to_string(p.preset) + "\n");
  ::sync();
}

uint8_t cam_switch_mask(uint8_t now_mask, const std::string& proc_modules, bool star6e_ko_present) {
  if (!star6e_ko_present) return 0;
  // /proc/modules lines start with the module name; the stock driver loads
  // as sensor_imx415_mipi, waybeam's as sensor_imx415_star6e.
  std::istringstream in(proc_modules);
  std::string line;
  bool stock_imx415 = false;
  while (std::getline(in, line)) {
    if (line.rfind("sensor_imx415_mipi ", 0) == 0) stock_imx415 = true;
    if (line.rfind("sensor_imx415_star6e ", 0) == 0) return 0;
  }
  if (!stock_imx415) return 0;
  return static_cast<uint8_t>(camera_offered_mask(kStar6eImx415Modes, kNumStar6eImx415Modes) &
                              ~now_mask);
}

}  // namespace mabur
