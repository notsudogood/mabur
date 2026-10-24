// Code-default vs shipped-bundle parity, shared by the three config tests.
//
// Each loader reports every key a file left unset as "a.b.key=<code default>"
// (the `defaulted` list). Loading a near-empty file therefore lists the code
// default of nearly every key; this compares each against the value the
// shipped bundle writes for the same key and returns the ones that differ.
//
// Why it matters: a config saved by an older image lacks the keys added
// since, and those keys take the CODE default, not the bundle's. When the two
// differ, an old config flies a setting nobody chose (gs13 found
// display.phase_lock off and radio.narrow_20 off that way).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "mabur/toml.h"

namespace bundle_parity {

// The bundle with every value removed: its [table] headers only, so each
// section is present and each key in it falls back to its code default (an
// empty file would skip whole sections, whose absent-section path is a
// different default). [[array]] blocks are dropped -- a list is not a
// default. `extra` adds lines under a header, for keys a loader requires.
inline std::string skeleton(const std::string& bundle_path,
                            const std::map<std::string, std::string>& extra = {}) {
  std::ifstream in(bundle_path);
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] != '[' || line.compare(b, 2, "[[") == 0) continue;
    const std::size_t e = line.find(']', b);
    const std::string name = line.substr(b + 1, e - b - 1);
    out << "[" << name << "]\n";
    auto it = extra.find(name);
    if (it != extra.end()) out << it->second << "\n";
  }
  return out.str();
}

// Text of a bundle value in the loader's to_text() shape: bools as
// true/false, strings bare, numbers compared numerically by the caller.
inline bool lookup(const mabur::toml::Value& root, const std::string& dotted,
                   const mabur::toml::Value** out) {
  const mabur::toml::Value* v = &root;
  std::size_t at = 0;
  while (true) {
    const std::size_t dot = dotted.find('.', at);
    const std::string part = dotted.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
    if (!v->is_object() || !v->contains(part)) return false;
    v = &v->at(part);
    if (dot == std::string::npos) break;
    at = dot + 1;
  }
  *out = v;
  return true;
}

inline bool same(const mabur::toml::Value& b, const std::string& code) {
  if (b.is_boolean()) return code == (b.get<bool>() ? "true" : "false");
  if (b.is_string()) return code == b.get<std::string>();
  if (b.is_number()) {
    char* end = nullptr;
    const double c = std::strtod(code.c_str(), &end);
    if (end == code.c_str()) return false;
    return std::fabs(c - b.get<double>()) < 1e-9;
  }
  return false;  // arrays and tables: the caller lists them as known differences
}

// "key=code" entries whose code default differs from the bundle's value,
// as "key: code <c>, bundle <b-line>". Keys absent from the bundle are
// skipped (the bundle-completeness tests own that). `known` holds the keys
// whose difference is deliberate; each must still differ, so a stale entry
// fails too.
inline std::vector<std::string> mismatches(const std::vector<std::string>& defaulted,
                                           const std::string& bundle_path,
                                           const std::set<std::string>& known) {
  const mabur::toml::Value bundle = mabur::toml::parse_toml_file(bundle_path);
  std::vector<std::string> out;
  std::set<std::string> seen_known;
  for (const std::string& d : defaulted) {
    const std::size_t eq = d.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = d.substr(0, eq), code = d.substr(eq + 1);
    const mabur::toml::Value* b = nullptr;
    if (!lookup(bundle, key, &b)) continue;
    const bool differs = !same(*b, code);
    if (known.count(key)) {
      seen_known.insert(key);
      if (!differs) out.push_back(key + ": listed as a known difference but code and bundle now agree");
      continue;
    }
    if (differs)
      out.push_back(key + ": code default " + code + ", bundle line " + std::to_string(b->line()));
  }
  for (const std::string& k : known)
    if (!seen_known.count(k)) out.push_back(k + ": listed as a known difference but never defaulted");
  for (const std::string& m : out) std::fprintf(stderr, "  parity: %s\n", m.c_str());
  return out;
}

}  // namespace bundle_parity
