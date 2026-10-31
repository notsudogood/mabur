#include "camera_switch.h"

#include <cstdio>
#include <sstream>
#include <vector>

#include "venc_cfg.h"

namespace mabur {

uint8_t camera_offered_mask(const CameraMode* modes, int n) {
  uint8_t mask = 0;
  for (const cam::Preset& p : cam::kPresets) {
    const CameraMode m = camera_mode_pick(p.fps, 0, 0, p.crop);
    if (camera_mode_find(modes, n, m) >= 0 && camera_mode_size_ok(m, p.w, p.h))
      mask = static_cast<uint8_t>(mask | (1u << p.id));
  }
  return mask;
}

CamDecision CameraSwitch::on_wish(uint8_t wish, uint8_t cur, uint8_t now_mask,
                                  uint8_t switch_mask, ArmView arm) {
  CamDecision d;
  if (wish == cam::kNoWish) {
    last_ = cam::kNoWish;
    return d;
  }
  if (wish == last_) return d;  // this request was decided already
  last_ = wish;
  if (wish == cur) return d;    // already running it: nothing to do
  if (!cam::preset(wish)) {
    d.action = CamAction::Refuse;
    d.refused = cam::State::Unavailable;
    return d;
  }
  // A switch reboots the drone: never while it may be flying.
  if (arm == ArmView::Armed || arm == ArmView::Stale) {
    d.action = CamAction::Refuse;
    d.refused = cam::State::NoArmState;
    return d;
  }
  const uint8_t bit = static_cast<uint8_t>(1u << wish);
  if (now_mask & bit) {
    d.action = CamAction::Apply;
  } else if (switch_mask & bit) {
    d.action = CamAction::Apply;
    d.driver_switch = true;
  } else {
    d.action = CamAction::Refuse;
    d.refused = cam::State::Unavailable;
  }
  return d;
}

namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

// The table header a line opens ("venc" for "[venc]  # x"), or "" if none.
std::string header_of(const std::string& line) {
  const std::string t = trim(line);
  if (t.size() < 2 || t[0] != '[') return "";
  const size_t close = t.find(']');
  if (close == std::string::npos) return "";
  return trim(t.substr(1, close - 1));
}

// The key a `key = value` line sets, or "".
std::string key_of(const std::string& line) {
  const std::string t = trim(line);
  if (t.empty() || t[0] == '#' || t[0] == '[') return "";
  const size_t eq = t.find('=');
  if (eq == std::string::npos) return "";
  return trim(t.substr(0, eq));
}

// Where the line's trailing comment starts, or npos. A '#' inside a quoted
// value is not one.
size_t comment_at(const std::string& line) {
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') quoted = !quoted;
    if (line[i] == '#' && !quoted) return i;
  }
  return std::string::npos;
}

// `line`'s key with a new value, its own indent, key spacing and comment
// kept -- the comment in its column while the new value fits before it.
std::string with_value(const std::string& line, const std::string& value) {
  const size_t eq = line.find('=');
  std::string out = line.substr(0, eq + 1) + " " + value;
  const size_t c = comment_at(line);
  if (c != std::string::npos) {
    out += c > out.size() ? std::string(c - out.size(), ' ') : std::string(" ");
    out += line.substr(c);
  }
  return out;
}

}  // namespace

uint8_t nearest_legal_slices(uint16_t height, uint8_t want) {
  auto legal = [&](int n) {
    return n == 1 || (n > 1 && n < 256 && venc_cfg_slice_rows(height, static_cast<uint8_t>(n)) != 0);
  };
  if (want >= 1 && legal(want)) return want;
  for (int d = 1; d < 256; ++d) {
    if (legal(want + d)) return static_cast<uint8_t>(want + d);
    if (want - d >= 1 && legal(want - d)) return static_cast<uint8_t>(want - d);
  }
  return 1;
}

std::string set_toml_values(const std::string& toml, const std::string& section,
                            const std::vector<std::pair<std::string, std::string>>& kv) {
  std::vector<std::string> lines;
  {
    std::string cur;
    for (char c : toml) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) lines.push_back(cur);
  }

  int header = -1;
  bool in_section = false;
  std::vector<bool> have(kv.size(), false);
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string hd = header_of(lines[i]);
    if (!hd.empty()) {
      in_section = hd == section;
      if (in_section) header = static_cast<int>(i);
      continue;
    }
    if (!in_section) continue;
    const std::string k = key_of(lines[i]);
    for (size_t j = 0; j < kv.size(); ++j) {
      if (k == kv[j].first) {
        lines[i] = with_value(lines[i], kv[j].second);
        have[j] = true;
      }
    }
  }

  std::vector<std::string> add;
  for (size_t j = 0; j < kv.size(); ++j)
    if (!have[j]) add.push_back(kv[j].first + " = " + kv[j].second);
  if (header < 0) {
    if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
    lines.push_back("[" + section + "]");
    lines.insert(lines.end(), add.begin(), add.end());
  } else {
    lines.insert(lines.begin() + header + 1, add.begin(), add.end());
  }

  std::string out;
  for (const std::string& l : lines) out += l + "\n";
  return out;
}

std::string set_venc_mode_text(const std::string& toml, uint16_t w, uint16_t h, uint16_t fps,
                               uint8_t slices) {
  std::vector<std::pair<std::string, std::string>> kv = {
      {"size", "\"" + std::to_string(w) + "x" + std::to_string(h) + "\""},
      {"fps", std::to_string(fps)}};
  if (slices != 0) kv.push_back({"slices", std::to_string(slices)});
  return set_toml_values(toml, "venc", kv);
}

std::string preset_config_text(const std::string& toml, const Config& cur, const cam::Preset& p) {
  const uint8_t slices = nearest_legal_slices(p.h, cur.venc.core.slices);
  std::string out =
      set_venc_mode_text(toml, p.w, p.h, p.fps, slices != cur.venc.core.slices ? slices : 0);
  // Every preset sets venc.sensor_crop, so leaving Crop 60 turns it off.
  out = set_toml_values(out, "venc", {{"sensor_crop", p.crop ? "true" : "false"}});
  // The SD recorder scales the same capture window, so its size must keep the
  // new picture's shape; a recording of another shape becomes the sensor
  // mode's own size (1728x816 for the wide 120 fps preset).
  const int rw = cur.record.width, rh = cur.record.height;
  if (cur.record.enable && rw > 0 &&
      static_cast<long>(rw) * p.h != static_cast<long>(p.w) * rh) {
    const CameraMode m = camera_mode_pick(p.fps, 0, 0, p.crop);
    out = set_toml_values(out, "record",
                          {{"size", "\"" + std::to_string(m.w) + "x" + std::to_string(m.h) + "\""}});
  }
  return out;
}

std::string format_cam_pending(const CamPending& p) {
  return "preset=" + std::to_string(p.preset) + "\ndriver_switched=" +
         (p.driver_switched ? "1" : "0") + "\n";
}

std::optional<CamPending> parse_cam_pending(const std::string& text) {
  CamPending p;
  bool have_preset = false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (k == "preset") {
      int id = -1;
      if (std::sscanf(v.c_str(), "%d", &id) != 1 || !cam::preset(static_cast<uint8_t>(id)) ||
          id < 0)
        return std::nullopt;
      p.preset = static_cast<uint8_t>(id);
      have_preset = true;
    } else if (k == "driver_switched") {
      p.driver_switched = v == "1";
    }
  }
  if (!have_preset) return std::nullopt;
  return p;
}

}  // namespace mabur
