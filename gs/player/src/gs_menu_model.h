#ifndef MABUR_PLAYER_GS_MENU_MODEL_H_
#define MABUR_PLAYER_GS_MENU_MODEL_H_

// The minimal GS on-screen menu (backlog item 9): three link settings
// driven by the 5-way stick, applied by restarting maburgs with
// /etc/maburgs.menu (gs/src/gs_menu.h). Pure state and text; MenuLayer
// draws it, main.cpp feeds it the buttons and performs Apply.
//
//   Channel hop   < On >        Left/Right (or Centre) toggles
//   Channel       < 132 >       the pin while hop is off
//   Bandwidth     < Wide >      Wide = the 80 MHz rungs; Normal = up to 40
//   Apply (link restarts ~5 s)
//   Close
//
// Centre opens it; Up/Down move; it closes on Close, after Apply, or after
// kIdleMs without a press.

#include <cstdint>
#include <string>
#include <vector>

namespace maburplay {

struct GsMenuSettings {
  bool hop = true;
  uint8_t channel = 0;
  bool wide = true;
  bool operator==(const GsMenuSettings& o) const {
    return hop == o.hop && channel == o.channel && wide == o.wide;
  }
};

class GsMenuModel {
 public:
  enum class Key { Up, Down, Left, Right, Center };
  enum class Action { None, Apply };
  static constexpr uint64_t kIdleMs = 20000;
  static constexpr uint64_t kStatusMs = 4000;

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
        sel_ = 0;
        pending_ = current_;
        ++version_;
      }
      return Action::None;
    }
    Action a = Action::None;
    switch (k) {
      case Key::Up: sel_ = (sel_ + kItems - 1) % kItems; break;
      case Key::Down: sel_ = (sel_ + 1) % kItems; break;
      case Key::Left: change_(-1); break;
      case Key::Right: change_(+1); break;
      case Key::Center:
        if (sel_ == kApply) {
          a = Action::Apply;
          current_ = pending_;
          open_ = false;
        } else if (sel_ == kClose) {
          open_ = false;
        } else {
          change_(+1);
        }
        break;
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
  int selected() const { return sel_; }
  const GsMenuSettings& pending() const { return pending_; }
  const GsMenuSettings& current() const { return current_; }

  // The rows to draw: a title, then one per item with "> " on the
  // selection; while closed, only the status row.
  std::vector<std::string> lines() const {
    std::vector<std::string> out;
    if (open_) {
      out.push_back("LINK SETTINGS");
      const auto row = [&](int i, const std::string& s) { out.push_back((sel_ == i ? "> " : "  ") + s); };
      row(kHop, std::string("Channel hop   < ") + (pending_.hop ? "On" : "Off") + " >");
      row(kChannel, "Channel       < " + std::to_string(static_cast<int>(pending_.channel)) + " >" +
                        (pending_.hop ? "  (hop picks)" : ""));
      row(kWidth, std::string("Bandwidth     ") +
                      (wide_ok_ ? (pending_.wide ? "< Wide >" : "< Normal >") : "Normal (fixed)"));
      row(kApply, pending_ == current_ ? "Apply (no change)" : "Apply (link restarts ~5 s)");
      row(kClose, "Close");
    }
    if (!status_.empty()) out.push_back(status_);
    return out;
  }

 private:
  static constexpr int kHop = 0, kChannel = 1, kWidth = 2, kApply = 3, kClose = 4, kItems = 5;

  void change_(int dir) {
    if (sel_ == kHop) {
      pending_.hop = !pending_.hop;
    } else if (sel_ == kChannel) {
      size_t i = 0;
      while (i < channels_.size() && channels_[i] != pending_.channel) ++i;
      if (i >= channels_.size()) i = 0;
      else i = (i + channels_.size() + static_cast<size_t>(dir > 0 ? 1 : channels_.size() - 1)) %
               channels_.size();
      pending_.channel = channels_[i];
    } else if (sel_ == kWidth && wide_ok_) {
      pending_.wide = !pending_.wide;
    }
  }

  std::vector<uint8_t> channels_;
  bool wide_ok_ = false;
  GsMenuSettings current_, pending_;
  bool open_ = false;
  int sel_ = 0;
  uint64_t version_ = 1;
  uint64_t last_key_ms_ = 0;
  std::string status_;
  uint64_t status_ms_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_GS_MENU_MODEL_H_
