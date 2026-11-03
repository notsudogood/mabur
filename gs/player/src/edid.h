#ifndef MABUR_PLAYER_EDID_H_
#define MABUR_PLAYER_EDID_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace maburplay {

// Who the HDMI port is driving, from the sink's EDID (the DRM connector's
// EDID blob, or sysfs). maburplay logs it at start and on every display
// hotplug, and display.trim_skip_sinks matches against it, so a sink that
// does not take the display trim can be left on the camera steer by name
// while every other sink keeps the trim (docs/phase-lock.md).
struct SinkInfo {
  bool valid = false;        // a base block with the fixed EDID header
  std::string mfr;           // PNP manufacturer id, 3 letters
  uint16_t product = 0;      // manufacturer's product code
  uint32_t serial = 0;       // numeric serial, often 0
  std::string name;          // monitor name descriptor (0xFC); "" when absent
  bool hdmi = false;         // the CTA extension carries the HDMI vendor block
  // YCbCr 4:2:0 for 1920x1080p60 (VIC 16), from the CTA-861 4:2:0 Video
  // Data Block ("only") and Capability Map ("also"). A sink that offers it
  // may be driven over a 4:2:0 link at half the pixel clock (hdmi_trim.h).
  enum class Y420 { kNo, kAlso, kOnly };
  Y420 y420_1080p60 = Y420::kNo;

  // "MFR-PPPP" (product code in hex): one id per model, present even when
  // the name descriptor is not.
  std::string id() const;
  // One line for the log: name, id, HDMI/DVI and the 4:2:0 note.
  std::string describe() const;
  // `pattern` equals the name or the id, ignoring case and surrounding
  // spaces. An invalid SinkInfo matches nothing.
  bool matches(const std::string& pattern) const;
};

// Parses a base block plus any CTA-861 extensions. Anything short, without
// the header, or truncated mid-block parses as far as it is whole.
SinkInfo parse_edid(const uint8_t* data, size_t len);

}  // namespace maburplay

#endif  // MABUR_PLAYER_EDID_H_
