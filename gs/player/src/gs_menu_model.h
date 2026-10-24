#ifndef MABUR_PLAYER_GS_MENU_MODEL_H_
#define MABUR_PLAYER_GS_MENU_MODEL_H_

// The GS on-screen menu (backlog item 9), laid out after the Caddx Ascent
// goggles' menu (mock-up v5): a tab bar, then per tab a title row and a
// grid of cells, a white outline on the selection. Three link settings,
// driven by the 5-way stick and applied by restarting maburgs with
// /etc/maburgs.menu (gs/src/gs_menu.h). Pure state and text; MenuLayer
// draws view(), main.cpp feeds it the buttons and performs Apply.
//
//   [ Channel ] [ Settings ]                  tabs: Left/Right
//   Channel                       Hop  (o-)   Left/Right/Centre toggle it
//   CH 36  | CH 100 | CH 116 | CH 132         Centre flies there (Hop off)
//   [ Apply ] [ Close ]
//   help line for the selection
//
//   Settings tab: one cell, Bandwidth < Wide >  (Left/Right/Centre change it)
//
// Up/Down move between rows (wrapping). Changes are staged until Apply,
// which restarts the link (~5 s); Close, or kIdleMs without a press, drops
// them.

#include <cstdint>
#include <string>
#include <vector>

#include "mabur/ht40.h"

namespace maburplay {

struct GsMenuSettings {
  bool hop = true;
  uint8_t channel = 0;
  bool wide = true;
  bool operator==(const GsMenuSettings& o) const {
    return hop == o.hop && channel == o.channel && wide == o.wide;
  }
};

// One box on screen. `active`: the light-grey fill (the open tab, the
// channel the link is on now). `marked`: the green dot (the channel Hop
// off flies on). `dim`: nothing to do there (Apply with no change).
struct GsMenuCell {
  std::string label, value;
  bool focus = false, active = false, marked = false, dim = false;
};

// Everything MenuLayer draws. `open` false = only `status`, if any.
struct GsMenuView {
  bool open = false;
  std::vector<GsMenuCell> tabs;
  std::string title;
  bool has_toggle = false;   // the Hop switch in the title row
  bool toggle_on = false;
  GsMenuCell toggle;
  std::vector<GsMenuCell> cells;
  int cols = 4;
  std::vector<GsMenuCell> buttons;   // Apply, Close
  std::string help;
  std::string status;
};

class GsMenuModel {
 public:
  enum class Key { Up, Down, Left, Right, Center };
  enum class Action { None, Apply };
  enum class Tab { Channel = 0, Settings = 1 };
  static constexpr uint64_t kIdleMs = 20000;
  static constexpr uint64_t kStatusMs = 4000;
  static constexpr int kTabs = 2;
  static constexpr int kCols = 4;   // channel cells per row

  // channels: radio.channels; wide_available: radio.width is 80 (Normal
  // is then the only other choice; at 20/40 Bandwidth is fixed).
  GsMenuModel(std::vector<uint8_t> channels, bool wide_available, GsMenuSettings current)
      : channels_(std::move(channels)), wide_ok_(wide_available), current_(current),
        pending_(current) {
    if (channels_.empty()) channels_.push_back(current.channel);
    if (!wide_ok_) current_.wide = pending_.wide = false;
  }

  Action key(Key k, uint64_t now_ms) {
    last_key_ms_ = now_ms;
    if (!open_) {
      if (k == Key::Center) {
        open_ = true;
        tab_ = Tab::Channel;
        row_ = kRowTabs;
        col_ = 0;
        pending_ = current_;
        ++version_;
      }
      return Action::None;
    }
    Action a = Action::None;
    const int rows = rows_();
    switch (k) {
      case Key::Up:
        row_ = (row_ + rows - 1) % rows;
        enter_row_();
        break;
      case Key::Down:
        row_ = (row_ + 1) % rows;
        enter_row_();
        break;
      case Key::Left: side_(-1); break;
      case Key::Right: side_(+1); break;
      case Key::Center: a = press_(); break;
    }
    ++version_;
    return a;
  }

  // Idle close and status expiry; bumps version() when the picture changes.
  void tick(uint64_t now_ms) {
    if (open_ && now_ms - last_key_ms_ >= kIdleMs) {
      open_ = false;
      ++version_;
    }
    if (!status_.empty() && now_ms - status_ms_ >= kStatusMs) {
      status_.clear();
      ++version_;
    }
  }

  // A one-line message shown (menu closed or open) for kStatusMs.
  void set_status(const std::string& s, uint64_t now_ms) {
    status_ = s;
    status_ms_ = now_ms;
    ++version_;
  }

  bool visible() const { return open_ || !status_.empty(); }
  bool open() const { return open_; }
  uint64_t version() const { return version_; }
  Tab tab() const { return tab_; }
  const GsMenuSettings& pending() const { return pending_; }
  const GsMenuSettings& current() const { return current_; }

  // What to draw. live_channel: the channel the link is on now (the stats
  // sideport's link.channel; 0 = unknown), filled light grey.
  GsMenuView view(uint8_t live_channel = 0) const {
    GsMenuView v;
    v.status = status_;
    v.open = open_;
    if (!open_) return v;
    const char* names[kTabs] = {"Channel", "Settings"};
    for (int t = 0; t < kTabs; ++t) {
      GsMenuCell c;
      c.label = names[t];
      c.active = static_cast<int>(tab_) == t;
      c.focus = row_ == kRowTabs && c.active;
      v.tabs.push_back(c);
    }
    if (tab_ == Tab::Channel) {
      v.title = "Channel";
      v.has_toggle = true;
      v.toggle_on = pending_.hop;
      v.toggle.label = "Hop";
      v.toggle.focus = row_ == kRowFirst;
      v.cols = static_cast<int>(channels_.size() < kCols ? channels_.size() : kCols);
      for (size_t i = 0; i < channels_.size(); ++i) {
        GsMenuCell c;
        c.label = "CH " + std::to_string(static_cast<int>(channels_[i]));
        c.value = range_text_(channels_[i]);
        c.active = live_channel != 0 && channels_[i] == live_channel;
        c.marked = !pending_.hop && channels_[i] == pending_.channel;
        c.focus = row_ == kRowFirst + 1 + static_cast<int>(i) / kCols &&
                  col_ == static_cast<int>(i) % kCols;
        v.cells.push_back(c);
      }
    } else {
      v.title = "Settings";
      v.cols = 3;
      GsMenuCell c;
      c.label = "Bandwidth";
      c.value = wide_ok_ ? (pending_.wide ? "< Wide >" : "< Normal >") : "Normal";
      c.dim = !wide_ok_;
      c.focus = row_ == kRowFirst;
      v.cells.push_back(c);
    }
    const bool changed = !(pending_ == current_);
    GsMenuCell apply, close;
    apply.label = changed ? "Apply" : "No change";
    apply.dim = !changed;
    apply.focus = row_ == buttons_row_() && col_ == 0;
    close.label = "Close";
    close.focus = row_ == buttons_row_() && col_ == 1;
    v.buttons = {apply, close};
    v.help = help_(live_channel);
    return v;
  }

 private:
  static constexpr int kRowTabs = 0, kRowFirst = 1;

  int channel_rows_() const { return static_cast<int>((channels_.size() + kCols - 1) / kCols); }
  // Rows of the open tab: the tab bar, its content, the buttons.
  int rows_() const { return 1 + (tab_ == Tab::Channel ? 1 + channel_rows_() : 1) + 1; }
  int buttons_row_() const { return rows_() - 1; }
  bool on_channels_() const {
    return tab_ == Tab::Channel && row_ > kRowFirst && row_ < buttons_row_();
  }
  int channel_index_() const { return (row_ - kRowFirst - 1) * kCols + col_; }
  int row_cols_() const {
    if (row_ == buttons_row_()) return 2;
    if (on_channels_()) {
      const int left = static_cast<int>(channels_.size()) - (row_ - kRowFirst - 1) * kCols;
      return left < kCols ? left : kCols;
    }
    return 1;
  }

  // Entering a row: the channel row lands on the chosen channel's cell,
  // the buttons on Apply; any row keeps the column inside its own width.
  void enter_row_() {
    if (row_ == buttons_row_()) col_ = 0;
    if (on_channels_()) {
      const int first = (row_ - kRowFirst - 1) * kCols;
      for (int i = 0; i < row_cols_(); ++i)
        if (channels_[static_cast<size_t>(first + i)] == pending_.channel) col_ = i;
    }
    const int n = row_cols_();
    if (col_ >= n) col_ = n - 1;
    if (col_ < 0) col_ = 0;
  }

  void side_(int dir) {
    if (row_ == kRowTabs) {
      tab_ = static_cast<Tab>((static_cast<int>(tab_) + kTabs + dir) % kTabs);
      col_ = 0;
    } else if (tab_ == Tab::Channel && row_ == kRowFirst) {
      pending_.hop = !pending_.hop;
    } else if (tab_ == Tab::Settings && row_ == kRowFirst) {
      if (wide_ok_) pending_.wide = !pending_.wide;
    } else {
      const int n = row_cols_();
      col_ = (col_ + n + dir) % n;
    }
  }

  Action press_() {
    if (row_ == kRowTabs) {
      row_ = kRowFirst;
      enter_row_();
    } else if (row_ == buttons_row_()) {
      if (col_ == 0 && !(pending_ == current_)) {
        current_ = pending_;
        open_ = false;
        return Action::Apply;
      }
      open_ = false;   // Close, or Apply with nothing to apply
    } else if (on_channels_()) {
      const int i = channel_index_();
      if (i >= 0 && i < static_cast<int>(channels_.size())) {
        pending_.channel = channels_[static_cast<size_t>(i)];
        pending_.hop = false;
      }
    } else {
      side_(+1);
    }
    return Action::None;
  }

  // The span of the channel's widest flight width: the 80 MHz block on
  // Wide, its 40 MHz pair on Normal (the 20 MHz channel off the grid).
  std::string range_text_(uint8_t ch) const {
    int lo = 0, hi = 0;
    if (pending_.wide && mabur::vht80_ok(ch)) {
      lo = 5000 + 5 * mabur::vht80_block_base(ch) - 10;
      hi = lo + 80;
    } else if (const uint8_t off = mabur::ht40_offset(ch)) {
      lo = 5000 + 5 * (off == 1 ? ch : ch - 4) - 10;
      hi = lo + 40;
    } else {
      lo = 5000 + 5 * ch - 10;
      hi = lo + 20;
    }
    return std::to_string(lo) + "-" + std::to_string(hi);
  }

  std::string help_(uint8_t live) const {
    const bool changed = !(pending_ == current_);
    if (row_ == kRowTabs) return "Left/Right: Channel or Settings. Down or Centre: into the tab.";
    if (row_ == buttons_row_()) {
      if (col_ == 0)
        return changed ? "Apply: save and restart the link (video back in about 5 s)."
                       : "Nothing changed yet.";
      return changed ? "Close: drop the changes." : "Close the menu.";
    }
    if (tab_ == Tab::Settings)
      return wide_ok_ ? "Normal: 20 and 40 MHz, up to 25 Mbps. Wide: adds 80 MHz, up to 40 Mbps. "
                        "Both switch width by themselves as the signal changes."
                      : "Bandwidth is fixed: radio.width in maburgs.toml must be 80 for Wide.";
    if (row_ == kRowFirst)
      return pending_.hop
                 ? "Hop on: the link flies on the cleanest of your channels and moves by itself "
                   "when one goes bad."
                 : "Hop off: the link stays on the channel with the green dot. Press a channel "
                   "to choose it.";
    const int i = channel_index_();
    const uint8_t ch = (i >= 0 && i < static_cast<int>(channels_.size()))
                           ? channels_[static_cast<size_t>(i)] : 0;
    std::string s = "Press to fly on CH " + std::to_string(static_cast<int>(ch)) + " only";
    s += pending_.hop ? " (turns Hop off)." : ".";
    if (live != 0) s += " Grey: the channel the link is on now.";
    return s;
  }

  std::vector<uint8_t> channels_;
  bool wide_ok_ = false;
  GsMenuSettings current_, pending_;
  bool open_ = false;
  Tab tab_ = Tab::Channel;
  int row_ = 0, col_ = 0;
  uint64_t version_ = 1;
  uint64_t last_key_ms_ = 0;
  std::string status_;
  uint64_t status_ms_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_GS_MENU_MODEL_H_
