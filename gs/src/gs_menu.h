#pragma once
// The GS on-screen menu's settings (backlog item 9, minimal): channel hop
// on/off, the channel to pin when it is off, and Bandwidth Normal/Wide.
// maburplay's menu writes them to kGsMenuFile and restarts maburgs;
// maburgs applies them over its TOML config at start. The file is the
// whole interface between the two processes, so either can be replaced,
// and a GS whose file is absent runs exactly as configured.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config.h"

namespace maburgs {

constexpr const char* kGsMenuFile = "/etc/maburgs.menu";

// Each field absent = leave the config's own value.
struct GsMenuChoice {
  std::optional<bool> hop;          // true = radio.channel "auto", false = pinned
  std::optional<uint8_t> channel;   // the pin when hop is off
  std::optional<bool> wide;         // false = Normal: no 80 MHz rungs
};

// "key=value" lines: hop=on|off, channel=N, bandwidth=normal|wide.
// Unknown keys and malformed lines are skipped (an older or newer menu
// must never stop the link from starting).
GsMenuChoice parse_gs_menu(const std::string& text);
std::string format_gs_menu(const GsMenuChoice& m);
std::optional<GsMenuChoice> read_gs_menu_file(const std::string& path);
// tmp file + fsync + rename, like the remembered channel.
bool write_gs_menu_file(const std::string& path, const GsMenuChoice& m);

// Bandwidth Normal: the ladder without its 80 MHz rungs, cards and the
// static pin never widen past 40. Returns the rungs removed.
int drop_80mhz_rungs(Config& cfg);

// Applies `m` to a loaded config. Returns one human line per change. A
// result that would not validate (a pin outside the set) leaves cfg
// untouched and fills *err.
std::vector<std::string> apply_gs_menu(Config& cfg, const GsMenuChoice& m, std::string* err);

}  // namespace maburgs
