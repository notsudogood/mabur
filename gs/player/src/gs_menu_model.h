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
//  |CH 36  |CH 100 |CH 116 |CH 132            Centre flies there (Hop off);
//                                             the bar: the last Scan's rating
//   [ Apply ] [ Scan ] [ Close ]              Scan: the Channel tab only
//   help line for the selection
//
//   Settings tab: Bandwidth < Wide > (Centre changes it), Calibrate (measure
//   this drone's TX-power walls, maburcal's run: ~2 min without video, only
//   while disarmed) and Remove cal (back to the radio's stock TX power,
//   live). Either calibration action is staged like any choice.
//
//   Playback tab (gs18): the newest recordings on the DVR card, two to a
//   row. Pressing one plays it on the goggles in place of the live video
//   (main.cpp, playback.h) -- only while the drone is disarmed or has no FC;
//   arming, or Left, brings the live video back.
//
//   Camera tab: the presets of mabur/camera_presets.h. Grey = the one the
//   drone runs, green dot = the one chosen, dim = this drone cannot (its
//   image lacks the sensor mode). Applying one asks the drone to switch: it
//   rewrites its config and reboots (~30 s without video), only while the
//   FC is not armed.
//
// Up/Down move between rows (wrapping). Changes are staged until Apply,
// which restarts the link (~5 s) for link settings, asks the drone for a
// camera preset, or starts / removes a calibration; Close, or kIdleMs
// without a press, drops them.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "mabur/camera_presets.h"
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
  // Channel tab: the last Scan's rating, drawn as a bar at the cell's left
  // edge: 0 clean (green), 1 some traffic (yellow), 2 busy (red); -1 none.
  int bar = -1;
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
  enum class Tab { Channel = 0, Settings = 1, Camera = 2, Playback = 3 };
  static constexpr uint64_t kIdleMs = 20000;
  static constexpr uint64_t kStatusMs = 4000;
  static constexpr int kTabs = 4;
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
        pending_cam_ = mabur::cam::kNoWish;
        pending_cal_ = CalAction::None;
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
    if (!status_.empty() && now_ms - status_ms_ >= status_dur_ms_) {
      status_.clear();
      ++version_;
    }
  }

  // The drone's camera as the stats sideport reports it (drone.camera):
  // the preset it runs (cam::kCustom / kUnknown otherwise), the presets on
  // offer now and after a driver switch. Bumps version() on a change.
  void set_camera(int cur, int now_mask, int switch_mask) {
    if (cur == cam_cur_ && now_mask == cam_now_ && switch_mask == cam_switch_) return;
    cam_cur_ = cur;
    cam_now_ = now_mask;
    cam_switch_ = switch_mask;
    ++version_;
  }

  // The drone's calibration as the stats sideport reports it (drone.cal,
  // calibration): whether it flies a calibrated TX-power shape (empty = not
  // reported), whether a run is going and its measured share (-1 = none).
  void set_calibration(std::optional<bool> calibrated, bool running, int pct) {
    if (calibrated == calibrated_ && running == cal_running_ && pct == cal_pct_) return;
    calibrated_ = calibrated;
    cal_running_ = running;
    cal_pct_ = pct;
    ++version_;
  }

  enum class CalAction { None, Start, Remove };
  CalAction take_cal_applied() {
    const CalAction a = cal_applied_;
    cal_applied_ = CalAction::None;
    return a;
  }
  CalAction pending_cal() const { return pending_cal_; }

  // What the sideport says about calibration, for calibration_event_text.
  struct CalSeen {
    std::string state, start, fail, off;
    int start_n = 0;
    std::optional<int> pct;
    std::optional<bool> calibrated;
    bool refused_armed = false;
  };
  // The status line for what changed between two sideport readings ("" for
  // nothing worth showing): a refused or failed start, the run's progress
  // in 10 % steps, its end, the "remove" request's outcome.
  static std::string calibration_event_text(const CalSeen& was, const CalSeen& now) {
    if (now.start_n != was.start_n && now.start.rfind("err ", 0) == 0)
      return "Calibration not started: " + now.start.substr(4);
    if (now.refused_armed && !was.refused_armed)
      return "Calibration refused: the drone is armed - disarm first";
    if (now.off != was.off) {
      if (now.off == "removed") return "Calibration removed: stock TX power";
      if (now.off == "timeout") return "Remove calibration: no answer from the drone";
    }
    if (now.state != was.state) {
      if (now.state == "await_ack") return "Calibrating: about 2 min without video";
      if (now.state == "done") return "Calibration done: video back in a few seconds";
      if (now.state == "failed")
        return "Calibration failed" + (now.fail.empty() ? std::string() : ": " + now.fail);
    }
    if (now.pct && (now.state == "sweep" || now.state == "verify") &&
        (!was.pct || *now.pct / 10 != *was.pct / 10))
      return "Calibrating: " + std::to_string(*now.pct) + " %";
    if (was.calibrated == false && now.calibrated == true) return "Calibration in use";
    return "";
  }

  // What the last Apply asked for, each consumed once: whether the link
  // settings changed (main.cpp restarts maburgs only then), and the camera
  // preset requested (cam::kNoWish: none).
  bool take_settings_applied() {
    const bool v = settings_applied_;
    settings_applied_ = false;
    return v;
  }
  uint8_t take_camera_applied() {
    const uint8_t v = camera_applied_;
    camera_applied_ = mabur::cam::kNoWish;
    return v;
  }
  uint8_t pending_camera() const { return pending_cam_; }

  // The status line for a camera_request.result (gs/src/camera_request.h);
  // "" for none worth showing.
  static std::string camera_result_text(const std::string& r) {
    if (r == "applying") return "Camera: drone restarting into the new mode";
    if (r == "switched") return "Camera: switched";
    if (r == "armed") return "Camera: refused while armed - disarm first";
    if (r == "unavailable") return "Camera: this drone cannot run that mode";
    if (r == "bad_config") return "Camera: refused - the drone's config does not take it";
    if (r == "rolled_back") return "Camera: no video in that mode - back on the old one";
    if (r == "timeout") return "Camera: no answer from the drone";
    return "";
  }

  // Channel tab Scan (gs18): maburgs' chscan block. state "idle", "running"
  // or "done"; per member its rating (-1 = not measured: the link's own
  // channel, or no scan yet) and worst NHM busy % (< 0 = none); age of the
  // last finished scan in s (-1 = none).
  struct ChannelRating {
    uint8_t ch = 0;
    int q = -1;
    double busy_pct = -1;
  };
  void set_channel_scan(const std::string& state, const std::vector<ChannelRating>& r, int age_s) {
    bool same = state == scan_state_ && age_s / 60 == scan_age_s_ / 60 && r.size() == ratings_.size();
    for (size_t i = 0; same && i < r.size(); ++i)
      same = r[i].ch == ratings_[i].ch && r[i].q == ratings_[i].q;
    scan_state_ = state;
    scan_age_s_ = age_s;
    ratings_ = r;
    if (!same) ++version_;
  }
  // The Scan button was pressed (consumed once): main.cpp sends "scan".
  bool take_scan_request() {
    const bool v = scan_requested_;
    scan_requested_ = false;
    return v;
  }
  // The status line for a chscan.result ("" for none worth showing).
  static std::string scan_result_text(const std::string& r) {
    if (r == "started") return "Scanning channels: about 5 s, the link stays up";
    if (r == "armed") return "Scan refused: the drone is armed - disarm first";
    if (r == "arm_stale") return "Scan refused: no arm state from the flight controller";
    if (r == "one_card") return "Scan needs two receive cards";
    if (r == "searching") return "Scan: no link to the drone yet";
    if (r == "running") return "Scan already running";
    if (r == "no_scout" || r == "card_down" || r == "nothing")
      return "Scan not possible: no spare receive card";
    return "";
  }

  // Playback tab (gs18). The list (main.cpp refreshes it on take_list_request,
  // the tab's every opening): name and duration, newest first.
  struct RecItem {
    std::string name;
    uint64_t duration_us = 0;
  };
  void set_recordings(const std::vector<RecItem>& r) {
    recs_ = r;
    ++version_;
  }
  // Whether a recording may play now, and why not ("" when it may).
  void set_playback_allowed(bool ok, const std::string& why) {
    if (ok == play_ok_ && why == play_why_) return;
    play_ok_ = ok;
    play_why_ = why;
    ++version_;
  }
  bool take_list_request() {
    const bool v = list_requested_;
    list_requested_ = false;
    return v;
  }
  // The recording pressed (index into the list), consumed once; -1 none.
  int take_play_request() {
    const int v = play_request_;
    play_request_ = -1;
    return v;
  }

  // A one-line message shown (menu closed or open) for kStatusMs, or
  // dur_ms.
  void set_status(const std::string& s, uint64_t now_ms, uint64_t dur_ms = kStatusMs) {
    status_ = s;
    status_ms_ = now_ms;
    status_dur_ms_ = dur_ms;
    ++version_;
  }

  // A line that stays up (menu closed or open) until replaced or cleared
  // with "": the first-boot floor calibration's progress. A status line
  // shows instead while one is up.
  void set_banner(const std::string& s) {
    if (s == banner_) return;
    banner_ = s;
    ++version_;
  }
  const std::string& banner() const { return banner_; }

  // First-boot floor calibration (maburgs' floorcal block + the drone's
  // floor block, gs_snapshot.h), for the two texts below.
  struct FloorCalSeen {
    std::string state, why;
    int width = 0, floor = 0, steps = 0;
    int clean_s = -1, need_s = 0;   // toward the width under test's mark
  };
  // The banner while a calibration runs, has paused mid-way or has just
  // finished; "" otherwise.
  static std::string floorcal_banner(const FloorCalSeen& f) {
    const std::string pre = "First boot calibration";
    const std::string w = std::to_string(f.width) + " MHz";
    if (f.state == "test") {
      std::string s = pre + ": testing " + w;
      if (f.clean_s >= 0 && f.need_s > 0)
        s += " (" + std::to_string(f.clean_s < f.need_s ? f.clean_s : f.need_s) + "/" +
             std::to_string(f.need_s) + " s clean)";
      return s + " - keep the drone disarmed";
    }
    if (f.state == "unstick")
      return pre + ": stall caught, floor now " + hex_(f.floor) + " - testing " + w + " again";
    if (f.state == "paused" && f.width > 0) {
      if (f.why == "armed") return pre + " paused: the drone is armed";
      if (f.why == "arm_stale") return pre + " paused: no arm state from the flight controller";
      if (f.why == "link") return pre + " paused: link lost";
      if (f.why == "spike")
        return pre + " waits: this channel has a radio spike - pick another channel";
      return pre + " paused";
    }
    if (f.state == "done")
      return pre + " finished: " + (f.floor > 0 ? "floor " + hex_(f.floor) : "no floor needed");
    return "";
  }
  // The status line for a calibration that just gave up; "" otherwise.
  static std::string floorcal_event(const FloorCalSeen& was, const FloorCalSeen& now) {
    if (now.state != "failed" || was.state == "failed") return "";
    if (now.why == "top")
      return "First boot calibration stopped: the drone still stalls at the highest floor (" +
             hex_(now.floor) + ")";
    const int w = now.width > 0 ? now.width : was.width > 0 ? was.width : 40;
    return "First boot calibration stopped: no clean " + std::to_string(w) +
           " MHz in a minute (retries after a GS restart)";
  }

  bool visible() const { return open_ || !status_.empty() || !banner_.empty(); }
  bool open() const { return open_; }
  uint64_t version() const { return version_; }
  Tab tab() const { return tab_; }
  const GsMenuSettings& pending() const { return pending_; }
  const GsMenuSettings& current() const { return current_; }

  // What to draw. live_channel: the channel the link is on now (the stats
  // sideport's link.channel; 0 = unknown), filled light grey.
  GsMenuView view(uint8_t live_channel = 0) const {
    GsMenuView v;
    v.status = status_.empty() ? banner_ : status_;
    v.open = open_;
    if (!open_) return v;
    const char* names[kTabs] = {"Channel", "Settings", "Camera", "Playback"};
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
        if (const ChannelRating* r = rating_(channels_[i])) c.bar = r->q;
        c.active = live_channel != 0 && channels_[i] == live_channel;
        c.marked = !pending_.hop && channels_[i] == pending_.channel;
        c.focus = row_ == kRowFirst + 1 + static_cast<int>(i) / kCols &&
                  col_ == static_cast<int>(i) % kCols;
        v.cells.push_back(c);
      }
    } else if (tab_ == Tab::Settings) {
      v.title = "Settings";
      v.cols = 3;
      GsMenuCell c;
      c.label = "Bandwidth";
      c.value = wide_ok_ ? (pending_.wide ? "< Wide >" : "< Normal >") : "Normal";
      c.dim = !wide_ok_;
      c.focus = row_ == kRowFirst && col_ == 0;
      v.cells.push_back(c);
      GsMenuCell cal;
      cal.label = "Calibrate";
      cal.value = cal_running_ ? (cal_pct_ >= 0 ? "running " + std::to_string(cal_pct_) + " %"
                                                : std::string("running"))
                               : std::string("measure TX");
      cal.dim = cal_running_;
      cal.active = cal_running_;
      cal.marked = pending_cal_ == CalAction::Start;
      cal.focus = row_ == kRowFirst && col_ == 1;
      v.cells.push_back(cal);
      GsMenuCell rm;
      rm.label = "Remove cal";
      rm.value = !calibrated_ ? "no link" : (*calibrated_ ? "in use" : "not in use");
      rm.dim = calibrated_ != true || cal_running_;
      rm.marked = pending_cal_ == CalAction::Remove;
      rm.focus = row_ == kRowFirst && col_ == 2;
      v.cells.push_back(rm);
    } else if (tab_ == Tab::Playback) {
      v.title = "Playback";
      v.cols = kRecCols;
      if (recs_.empty()) {
        GsMenuCell c;
        c.label = "No recordings";
        c.dim = true;
        c.focus = row_ == kRowFirst;
        v.cells.push_back(c);
      }
      for (size_t i = 0; i < recs_.size(); ++i) {
        GsMenuCell c;
        c.label = recs_[i].name;
        c.value = clock_(recs_[i].duration_us);
        c.dim = !play_ok_;
        c.focus = on_recs_() && rec_index_() == static_cast<int>(i);
        v.cells.push_back(c);
      }
    } else {
      v.title = "Camera";
      v.cols = static_cast<int>(mabur::cam::kNumPresets);
      for (const mabur::cam::Preset& p : mabur::cam::kPresets) {
        GsMenuCell c;
        c.label = p.label;
        const int bit = 1 << p.id;
        if (cam_now_ & bit)
          c.value = p.view;
        else if (cam_switch_ & bit)
          c.value = "new driver";
        else
          c.value = "not here";
        c.dim = !((cam_now_ | cam_switch_) & bit);
        c.active = cam_cur_ == p.id;
        c.marked = pending_cam_ == p.id;
        c.focus = row_ == kRowFirst && col_ == p.id;
        v.cells.push_back(c);
      }
    }
    const bool changed = !(pending_ == current_) || pending_cam_ != mabur::cam::kNoWish ||
                         pending_cal_ != CalAction::None;
    GsMenuCell apply, close;
    apply.label = changed ? "Apply" : "No change";
    apply.dim = !changed;
    apply.focus = row_ == buttons_row_() && col_ == 0;
    close.label = "Close";
    close.focus = row_ == buttons_row_() && col_ == close_col_();
    if (tab_ == Tab::Channel) {
      GsMenuCell scan;
      scan.label = scan_state_ == "running" ? "Scanning" : "Scan";
      scan.dim = scan_state_ == "running";
      scan.focus = row_ == buttons_row_() && col_ == 1;
      v.buttons = {apply, scan, close};
    } else {
      v.buttons = {apply, close};
    }
    v.help = help_(live_channel);
    return v;
  }

 private:
  static constexpr int kRowTabs = 0, kRowFirst = 1;

  int channel_rows_() const { return static_cast<int>((channels_.size() + kCols - 1) / kCols); }
  // Rows of the open tab: the tab bar, its content, the buttons.
  int rows_() const {
    const int content = tab_ == Tab::Channel    ? 1 + channel_rows_()
                        : tab_ == Tab::Playback ? rec_rows_()
                                                : 1;
    return 1 + content + 1;
  }
  static constexpr int kRecCols = 2;
  int rec_rows_() const {
    return recs_.empty() ? 1 : static_cast<int>((recs_.size() + kRecCols - 1) / kRecCols);
  }
  bool on_recs_() const {
    return tab_ == Tab::Playback && row_ >= kRowFirst && row_ < buttons_row_();
  }
  int rec_index_() const { return (row_ - kRowFirst) * kRecCols + col_; }
  static std::string clock_(uint64_t us) {
    const uint64_t s = us / 1000000;
    char b[24];
    if (s >= 3600)
      std::snprintf(b, sizeof(b), "%u:%02u:%02u", static_cast<unsigned>(s / 3600),
                    static_cast<unsigned>(s / 60 % 60), static_cast<unsigned>(s % 60));
    else
      std::snprintf(b, sizeof(b), "%u:%02u", static_cast<unsigned>(s / 60),
                    static_cast<unsigned>(s % 60));
    return b;
  }
  bool on_presets_() const { return tab_ == Tab::Camera && row_ == kRowFirst; }
  int buttons_row_() const { return rows_() - 1; }
  bool on_channels_() const {
    return tab_ == Tab::Channel && row_ > kRowFirst && row_ < buttons_row_();
  }
  int channel_index_() const { return (row_ - kRowFirst - 1) * kCols + col_; }
  // The buttons row: Apply, [Scan on the Channel tab,] Close.
  int close_col_() const { return tab_ == Tab::Channel ? 2 : 1; }
  const ChannelRating* rating_(uint8_t ch) const {
    for (const ChannelRating& r : ratings_)
      if (r.ch == ch) return &r;
    return nullptr;
  }
  int row_cols_() const {
    if (row_ == buttons_row_()) return close_col_() + 1;
    if (on_presets_()) return static_cast<int>(mabur::cam::kNumPresets);
    if (tab_ == Tab::Settings && row_ == kRowFirst) return 3;
    if (on_channels_()) {
      const int left = static_cast<int>(channels_.size()) - (row_ - kRowFirst - 1) * kCols;
      return left < kCols ? left : kCols;
    }
    if (on_recs_()) {
      if (recs_.empty()) return 1;
      const int left = static_cast<int>(recs_.size()) - (row_ - kRowFirst) * kRecCols;
      return left < kRecCols ? left : kRecCols;
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
    if (on_presets_()) {
      if (pending_cam_ != mabur::cam::kNoWish)
        col_ = pending_cam_;
      else if (mabur::cam::preset(static_cast<uint8_t>(cam_cur_ < 0 ? 0xFF : cam_cur_)))
        col_ = cam_cur_;
    }
    const int n = row_cols_();
    if (col_ >= n) col_ = n - 1;
    if (col_ < 0) col_ = 0;
  }

  void side_(int dir) {
    if (row_ == kRowTabs) {
      tab_ = static_cast<Tab>((static_cast<int>(tab_) + kTabs + dir) % kTabs);
      col_ = 0;
      if (tab_ == Tab::Playback) list_requested_ = true;
    } else if (tab_ == Tab::Channel && row_ == kRowFirst) {
      pending_.hop = !pending_.hop;
    } else if (on_presets_() || (tab_ == Tab::Settings && row_ == kRowFirst)) {
      const int n = row_cols_();
      col_ = (col_ + n + dir) % n;
    } else {
      const int n = row_cols_();
      col_ = (col_ + n + dir) % n;
    }
  }

  Action press_() {
    if (row_ == kRowTabs) {
      row_ = kRowFirst;
      enter_row_();
    } else if (row_ == buttons_row_() && tab_ == Tab::Channel && col_ == 1) {
      // Scan: the menu stays open so the bars fill in where you look.
      if (scan_state_ != "running") scan_requested_ = true;
    } else if (row_ == buttons_row_()) {
      if (col_ == 0 && (!(pending_ == current_) || pending_cam_ != mabur::cam::kNoWish ||
                        pending_cal_ != CalAction::None)) {
        settings_applied_ = !(pending_ == current_);
        camera_applied_ = pending_cam_;
        cal_applied_ = pending_cal_;
        current_ = pending_;
        pending_cam_ = mabur::cam::kNoWish;
        pending_cal_ = CalAction::None;
        open_ = false;
        return Action::Apply;
      }
      open_ = false;   // Close, or Apply with nothing to apply
    } else if (tab_ == Tab::Settings && row_ == kRowFirst) {
      // Bandwidth toggles; a calibration action is chosen or, pressed again,
      // cleared (one at a time). Calibrate while a run is going, or Remove
      // on a drone that is not calibrated (or has not said), does nothing.
      if (col_ == 0) {
        if (wide_ok_) pending_.wide = !pending_.wide;
      } else if (col_ == 1) {
        pending_cal_ = pending_cal_ == CalAction::Start ? CalAction::None
                       : cal_running_                  ? pending_cal_
                                                       : CalAction::Start;
      } else {
        pending_cal_ = pending_cal_ == CalAction::Remove                 ? CalAction::None
                       : (calibrated_ == true && !cal_running_)           ? CalAction::Remove
                                                                          : pending_cal_;
      }
    } else if (on_presets_()) {
      // A preset this drone can run (now or after a driver switch) becomes
      // the choice; the running one, or pressing the choice again, clears it.
      const int bit = 1 << col_;
      if (col_ == cam_cur_ || pending_cam_ == col_)
        pending_cam_ = mabur::cam::kNoWish;
      else if ((cam_now_ | cam_switch_) & bit)
        pending_cam_ = static_cast<uint8_t>(col_);
    } else if (on_channels_()) {
      const int i = channel_index_();
      if (i >= 0 && i < static_cast<int>(channels_.size())) {
        pending_.channel = channels_[static_cast<size_t>(i)];
        pending_.hop = false;
      }
    } else if (on_recs_()) {
      // A recording plays at once (no Apply): the menu closes over it.
      const int i = rec_index_();
      if (i >= 0 && i < static_cast<int>(recs_.size())) {
        if (play_ok_) {
          play_request_ = i;
          open_ = false;
        } else {
          status_ = "Playback refused: " + play_why_;
          status_ms_ = last_key_ms_;
          status_dur_ms_ = kStatusMs;
        }
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
    const bool link_changed = !(pending_ == current_);
    const bool cam_changed = pending_cam_ != mabur::cam::kNoWish;
    const bool cal_changed = pending_cal_ != CalAction::None;
    const bool changed = link_changed || cam_changed || cal_changed;
    if (row_ == kRowTabs)
      return "Left/Right: Channel, Settings, Camera or Playback. Down or Centre: into the tab.";
    if (row_ == buttons_row_() && tab_ == Tab::Channel && col_ == 1) {
      if (scan_state_ == "running") return "Scanning every channel with the spare card...";
      return "Scan: rate every channel's traffic with the spare card (about 5 s, the link stays "
             "up; not while armed). Green clean, yellow some, red busy.";
    }
    if (row_ == buttons_row_()) {
      if (col_ == 0) {
        if (cal_changed && (link_changed || cam_changed))
          return "Apply: link and camera changes first; apply the calibration again once video "
                 "is back.";
        if (pending_cal_ == CalAction::Start)
          return "Apply: start calibrating (about 2 min without video; refused while armed).";
        if (pending_cal_ == CalAction::Remove)
          return "Apply: remove the calibration (stock TX power, live, no video gap).";
        if (link_changed && cam_changed)
          return "Apply: restart the link, then the drone switches camera (no picture for "
                 "about 30 s).";
        if (cam_changed)
          return "Apply: the drone switches camera and restarts (no picture for about 30 s; "
                 "refused while armed).";
        return changed ? "Apply: save and restart the link (video back in about 5 s)."
                       : "Nothing changed yet.";
      }
      return changed ? "Close: drop the changes." : "Close the menu.";
    }
    if (tab_ == Tab::Playback) {
      if (recs_.empty()) return "No recordings on the DVR card yet.";
      const int i = rec_index_();
      if (i < 0 || i >= static_cast<int>(recs_.size())) return "";
      if (!play_ok_) return "Playback is off: " + play_why_ + ".";
      return "Press to play " + recs_[static_cast<size_t>(i)].name + " (" +
             clock_(recs_[static_cast<size_t>(i)].duration_us) +
             ") here. Live video stops while it plays; Left or arming the drone brings it back.";
    }
    if (tab_ == Tab::Camera) {
      if (cam_now_ == 0 && cam_switch_ == 0)
        return "The drone has not reported its camera (no link yet, or an older drone image).";
      const mabur::cam::Preset* p = mabur::cam::preset(static_cast<uint8_t>(col_));
      if (!p) return "";
      const int bit = 1 << p->id;
      std::string s = std::string(p->label) + ": " + (p->about ? p->about : p->view) + ".";
      if (col_ == cam_cur_) return s + " The drone runs this now.";
      if (!((cam_now_ | cam_switch_) & bit))
        return s + " Not on this drone: its image has no camera mode for it.";
      if (!(cam_now_ & bit)) s += " Also switches the drone to waybeam's camera driver.";
      if (p->fps > 60) s += " Smoothest on a screen at that rate.";
      return s;
    }
    if (tab_ == Tab::Settings && col_ == 1)
      return cal_running_ ? "A calibration is running: no video until it ends."
                          : "Calibrate: measure this drone's TX power limits (about 2 min without "
                            "video). Disarmed, props off, link up. Replaces any calibration.";
    if (tab_ == Tab::Settings && col_ == 2) {
      if (!calibrated_) return "The drone has not reported its calibration (no link yet).";
      return *calibrated_ ? "Remove cal: back to the radio's stock TX power, live. Calibrate again "
                            "to measure it anew."
                          : "This drone flies its radio's stock TX power: nothing to remove.";
    }
    if (tab_ == Tab::Settings)
      return wide_ok_ ? "Normal: 20 and 40 MHz, up to 25 Mbps. Wide: adds 80 MHz, up to 45 Mbps. "
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
    if (const ChannelRating* r = rating_(ch)) {
      if (r->q >= 0) {
        static const char* kWords[3] = {"clean", "some traffic", "busy"};
        s += std::string(" Last scan: ") + kWords[r->q];
        if (r->busy_pct >= 0) s += " (" + std::to_string(static_cast<int>(r->busy_pct + 0.5)) + " % busy)";
        if (scan_age_s_ >= 60) s += ", " + std::to_string(scan_age_s_ / 60) + " min ago";
        s += ".";
      } else if (scan_state_ == "done" && ch == live) {
        s += " Not scanned: the link is on it.";
      }
    } else if (live != 0) {
      s += " Grey: the channel the link is on now.";
    }
    return s;
  }

  std::vector<uint8_t> channels_;
  bool wide_ok_ = false;
  int cam_cur_ = -1;            // the preset the drone runs (-1 = not reported)
  int cam_now_ = 0, cam_switch_ = 0;
  uint8_t pending_cam_ = mabur::cam::kNoWish;
  std::optional<bool> calibrated_;
  bool cal_running_ = false;
  int cal_pct_ = -1;
  CalAction pending_cal_ = CalAction::None;
  CalAction cal_applied_ = CalAction::None;
  bool settings_applied_ = false;
  uint8_t camera_applied_ = mabur::cam::kNoWish;
  GsMenuSettings current_, pending_;
  bool open_ = false;
  Tab tab_ = Tab::Channel;
  int row_ = 0, col_ = 0;
  uint64_t version_ = 1;
  uint64_t last_key_ms_ = 0;
  std::string status_;
  uint64_t status_ms_ = 0;
  uint64_t status_dur_ms_ = kStatusMs;
  std::string banner_;
  std::vector<RecItem> recs_;
  bool play_ok_ = false;
  std::string play_why_ = "no word from the drone yet";
  bool list_requested_ = false;
  int play_request_ = -1;
  std::string scan_state_;
  int scan_age_s_ = -1;
  std::vector<ChannelRating> ratings_;
  bool scan_requested_ = false;

  static std::string hex_(int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02x", v & 0xff);
    return b;
  }
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_GS_MENU_MODEL_H_
