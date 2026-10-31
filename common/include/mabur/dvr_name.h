#ifndef MABUR_DVR_NAME_H_
#define MABUR_DVR_NAME_H_

#include <string>

namespace mabur {

// Mints DVR recording paths: `<dir>/record-NNNN.mp4`, %04d, zero-based,
// or with `folder` (maburplay's dvr.folders, gs18)
// `<dir>/record-NNNN/record-NNNN.mp4`: one folder per recording, holding
// the video and its .osd/.srt/.ass sidecars under the same name.
//
// No timestamp, by design. The GS RTC is wrong at boot, so a stamped name
// carries no information and sorts arbitrarily across boots -- the same
// reason CtlLog indexes its files (`ctl-NNNN_<date>.log`) and the stats
// recorder writes `flight-NNNN.jsonl`. The index is one past the highest
// `record-NNNN` already on the card, so it survives reboots and never
// overwrites an earlier flight. Legacy `record_<date>.mp4` files do not
// match the pattern: they are ignored and keep their names.
//
// The high-water mark is not redundant with the scan. NEITHER writer
// creates the file when the name is minted -- burned mode opens the mux on
// the encoder thread at the first encoded frame, raw mode at the next
// IRAP sync point (up to ~2 s later) -- so a stop/start pair inside that
// window would scan an unchanged directory twice and hand out one index
// twice, and both writers open "wb". The record button makes that a
// double-tap away (kDebounceMs is 50).
class DvrNamer {
 public:
  // Main-loop-thread only (the ring sink and rec_start's
  // start_burn_if_needed), so no synchronisation.
  // The scan counts files and folders alike (both are `record-NNNN...`),
  // so a card that holds both layouts keeps one numbering. The folder is
  // NOT created here: make_recording_dir() does that where the file is
  // opened, so a press that never reached a keyframe leaves no empty
  // folder, and this stays a read on the main loop.
  std::string next(const std::string& dir, bool folder = false);

 private:
  int last_issued_ = -1;
};

// Creates the folder `path` sits in when that folder is the recording's
// own: named like the file without its extension (record-0012/record-0012.mp4
// or .../record-0012.osd). Any other layout is left alone -- a flat
// `<dir>/record-0012.mp4` never creates `<dir>`, so a missing card still
// fails at the open, loudly, instead of recording into the root filesystem.
// One level only, never the whole chain. False when the folder should
// exist and does not afterwards.
bool make_recording_dir(const std::string& path);

}  // namespace mabur

#endif  // MABUR_DVR_NAME_H_
