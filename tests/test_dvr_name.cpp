#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "mabur/dvr_name.h"
#include "mtest.h"

using mabur::DvrNamer;

namespace {

// Each test gets its own empty directory: the namer's scan reads whatever
// is on disk, so a shared scratch dir would couple the cases.
std::string fresh_dir(const char* name) {
  const char* tmp = std::getenv("TMPDIR");
  std::string dir = std::string(tmp ? tmp : "/tmp") + "/dvrname_" + name;
  // Best-effort teardown of a previous run: only the files these tests
  // create, so a stray unlink cannot escape the scratch dir.
  for (const char* f : {"record-0000.mp4", "record-0007.mp4", "record-0008.mp4",
                        "record-0123.mp4", "record-9999.mp4",
                        "record_2026-08-26_12-00-00.mp4", "flight-0003.jsonl",
                        "ctl-0002_20260826.log", "notes.txt"}) {
    ::unlink((dir + "/" + f).c_str());
  }
  ::mkdir(dir.c_str(), 0777);
  return dir;
}

void touch(const std::string& dir, const char* name) {
  std::FILE* f = std::fopen((dir + "/" + name).c_str(), "wb");
  REQUIRE(f != nullptr);
  std::fclose(f);
}

}  // namespace

TEST(dvr_name_empty_dir_starts_at_zero) {
  const std::string dir = fresh_dir("empty");
  DvrNamer n;
  CHECK(n.next(dir) == dir + "/record-0000.mp4");
}

TEST(dvr_name_continues_past_highest_on_disk) {
  const std::string dir = fresh_dir("existing");
  touch(dir, "record-0007.mp4");
  DvrNamer n;
  CHECK(n.next(dir) == dir + "/record-0008.mp4");
}

// The reason the namer cannot be a bare directory scan: NEITHER writer
// creates the file when the name is minted. Burned mode opens the mux on
// the encoder thread at the first encoded frame; raw mode waits for the
// next sync point, up to ~2 s. A stop/start pair inside that window scans
// an unchanged directory twice, so the high-water mark is what keeps the
// second name from colliding with the first.
TEST(dvr_name_second_call_does_not_reuse_index_before_file_exists) {
  const std::string dir = fresh_dir("highwater");
  touch(dir, "record-0007.mp4");
  DvrNamer n;
  CHECK(n.next(dir) == dir + "/record-0008.mp4");
  CHECK(n.next(dir) == dir + "/record-0009.mp4");  // nothing written yet
  CHECK(n.next(dir) == dir + "/record-0010.mp4");
}

// The DVR card is shared with the stats/ctl recorders and with recordings
// from before this naming (record_<date>.mp4). None of them are ours, and
// the date-stamped ones in particular must not be read as index 2026.
TEST(dvr_name_ignores_foreign_and_legacy_files) {
  const std::string dir = fresh_dir("foreign");
  touch(dir, "flight-0003.jsonl");
  touch(dir, "ctl-0002_20260826.log");
  touch(dir, "notes.txt");
  touch(dir, "record_2026-08-26_12-00-00.mp4");
  DvrNamer n;
  CHECK(n.next(dir) == dir + "/record-0000.mp4");
}

// An unreadable dvr.dir must not stop a recording from being named: the
// open that follows will fail loudly on its own, and a namer that refused
// would turn a missing SD card into a silent no-name path.
TEST(dvr_name_survives_unreadable_dir) {
  const std::string dir = "/nonexistent-dvr-dir";
  DvrNamer n;
  CHECK(n.next(dir) == dir + "/record-0000.mp4");
  CHECK(n.next(dir) == dir + "/record-0001.mp4");
}

// Width is a minimum, not a truncation: past 9999 the name grows a digit
// rather than wrapping onto an existing file.
// dvr.folders (gs18): record-NNNN/record-NNNN.mp4. The scan counts the
// folders and older side-by-side files alike, so switching layouts keeps
// one numbering. Revert (folder ignored by next): record-0008.mp4 flat.
TEST(dvr_name_folder_mode_continues_one_numbering) {
  const std::string dir = fresh_dir("folders");
  ::rmdir((dir + "/record-0009").c_str());
  ::rmdir((dir + "/record-0010").c_str());
  touch(dir, "record-0007.mp4");                 // a flat file from before gs18
  DvrNamer n;
  CHECK(n.next(dir, true) == dir + "/record-0008/record-0008.mp4");
  REQUIRE(::mkdir((dir + "/record-0009").c_str(), 0755) == 0);
  DvrNamer fresh;                                // a new boot: only the card counts
  CHECK(fresh.next(dir, true) == dir + "/record-0010/record-0010.mp4");
  CHECK(fresh.next(dir, false) == dir + "/record-0011.mp4");
  ::rmdir((dir + "/record-0009").c_str());
}

// The folder is made where the file is opened, and only when it is the
// recording's own: a flat path never creates its dir, so a missing card
// still fails at the open. Revert (mkdir of any parent): the flat case
// creates /nonexistent-like dirs and the third CHECK fails.
TEST(make_recording_dir_only_makes_the_recordings_own_folder) {
  const std::string dir = fresh_dir("mkdir");
  ::rmdir((dir + "/record-0004").c_str());
  CHECK(mabur::make_recording_dir(dir + "/record-0004/record-0004.mp4"));
  struct stat st {};
  CHECK(::stat((dir + "/record-0004").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(mabur::make_recording_dir(dir + "/record-0004/record-0004.osd"));   // exists: fine
  const std::string missing = dir + "/no-card";
  ::rmdir(missing.c_str());
  CHECK(mabur::make_recording_dir(missing + "/record-0005.mp4"));          // flat: nothing to do
  CHECK(::stat(missing.c_str(), &st) != 0);                               // ...and nothing made
  CHECK(!mabur::make_recording_dir("/nonexistent-dvr-dir/record-0001/record-0001.mp4"));
  ::rmdir((dir + "/record-0004").c_str());
}

TEST(dvr_name_widens_past_four_digits) {
  const std::string dir = fresh_dir("wide");
  touch(dir, "record-9999.mp4");
  DvrNamer n;
  CHECK(n.next(dir) == dir + "/record-10000.mp4");
}
MTEST_MAIN
