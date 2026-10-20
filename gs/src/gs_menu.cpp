#include "gs_menu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include "mabur/channel_set.h"

namespace maburgs {

namespace {
std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}
}  // namespace

GsMenuChoice parse_gs_menu(const std::string& text) {
  GsMenuChoice m;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (k == "hop") {
      if (v == "on") m.hop = true;
      else if (v == "off") m.hop = false;
    } else if (k == "channel") {
      char* end = nullptr;
      const long ch = std::strtol(v.c_str(), &end, 10);
      if (end && *end == '\0' && ch >= 1 && ch <= mabur::kMaxChannel)
        m.channel = static_cast<uint8_t>(ch);
    } else if (k == "bandwidth") {
      if (v == "wide") m.wide = true;
      else if (v == "normal") m.wide = false;
    }
  }
  return m;
}

std::string format_gs_menu(const GsMenuChoice& m) {
  std::string s = "# written by the maburplay menu; delete to use /etc/maburgs.toml as is\n";
  if (m.hop) s += std::string("hop=") + (*m.hop ? "on" : "off") + "\n";
  if (m.channel) s += "channel=" + std::to_string(static_cast<int>(*m.channel)) + "\n";
  if (m.wide) s += std::string("bandwidth=") + (*m.wide ? "wide" : "normal") + "\n";
  return s;
}

std::optional<GsMenuChoice> read_gs_menu_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  return parse_gs_menu(ss.str());
}

bool write_gs_menu_file(const std::string& path, const GsMenuChoice& m) {
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return false;
  const std::string s = format_gs_menu(m);
  const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size() && std::fflush(f) == 0 &&
                  ::fsync(::fileno(f)) == 0;
  if (std::fclose(f) != 0 || !ok) {
    std::remove(tmp.c_str());
    return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

int drop_80mhz_rungs(Config& cfg) {
  auto& l = cfg.link.ladder_cfg.ladder;
  const auto n = l.size();
  l.erase(std::remove_if(l.begin(), l.end(), [](const auto& r) { return r.bw == 80; }), l.end());
  if (cfg.radio.width == 80) cfg.radio.width = 40;
  if (cfg.link.static_bw == 80) cfg.link.static_bw = 40;
  return static_cast<int>(n - l.size());
}

std::vector<std::string> apply_gs_menu(Config& cfg, const GsMenuChoice& m, std::string* err) {
  Config c = cfg;
  std::vector<std::string> lines;
  if (m.wide.has_value() && !*m.wide) {
    if (const int n = drop_80mhz_rungs(c))
      lines.push_back("bandwidth Normal: " + std::to_string(n) + " 80 MHz rung(s) left out");
  }
  if (m.hop.has_value()) {
    if (*m.hop) {
      if (c.radio.pin) lines.push_back("channel hop on (was pinned to " +
                                       std::to_string(static_cast<int>(*c.radio.pin)) + ")");
      c.radio.pin.reset();
    } else {
      const uint8_t ch = m.channel.value_or(c.radio.pin.value_or(c.radio.channels.front()));
      if (!mabur::channel_set_member(c.radio.channels, ch)) {
        if (err) *err = "menu pin " + std::to_string(static_cast<int>(ch)) +
                        " is not a member of radio.channels";
        return {};
      }
      c.radio.pin = ch;
      lines.push_back("channel hop off, pinned to " + std::to_string(static_cast<int>(ch)));
    }
  }
  if (c.link.ladder_cfg.ladder.empty()) {
    if (err) *err = "menu Normal would leave no rung";
    return {};
  }
  if (auto e = mabur::channel_set_issue(c.radio.channels, c.radio.width, "radio.channels")) {
    if (err) *err = "menu: " + e->field + ": " + e->why;
    return {};
  }
  if (auto e = link_width_issue(c.link, c.radio.width)) {
    if (err) *err = "menu: " + e->field + ": " + e->why;
    return {};
  }
  cfg = c;
  return lines;
}

}  // namespace maburgs
