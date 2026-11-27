// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 91: shared textures and fences inside the app's one process (iOS). The Windows
// half's pure parts, so tests/test_share.cpp checks them as they are:
// - the share name that carries a share-table key in D3DKMT's runtime data (ShareName,
//   ParseShareName);
// - the description an opener asks for against the shared texture's own (DescriptionDiff);
// - the lines: each kind once per process, then counted, and every 10 s one line per kind that
//   came again (ShareLog).
// The table itself is on the unix side (winemetal/unix/dxmt_share_table.h).

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace dxmt::share {

/* The runtime data's name field (SharedResourceData::mach_port_name, the fence's escape) */
constexpr size_t kNameSize = 54;
constexpr const char *kNamePrefix = "DXMT_ios_share_";

inline void
ShareName(uint32_t key, char (&out)[kNameSize]) {
  std::memset(out, 0, sizeof(out));
  std::snprintf(out, sizeof(out), "%s%08x", kNamePrefix, key);
}

/* The key in a name ShareName wrote; false for any other name (a macOS one, a short or
   unterminated one, key 0). */
inline bool
ParseShareName(const char *name, size_t size, uint32_t *key) {
  *key = 0;
  const size_t prefix = std::strlen(kNamePrefix);
  if (!name || size < prefix + 9 || std::memchr(name, 0, size) == nullptr)
    return false;
  if (std::strncmp(name, kNamePrefix, prefix) != 0 || std::strlen(name) != prefix + 8)
    return false;
  uint32_t value = 0;
  for (size_t i = prefix; i < prefix + 8; i++) {
    const char c = name[i];
    uint32_t digit;
    if (c >= '0' && c <= '9')
      digit = uint32_t(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = uint32_t(c - 'a' + 10);
    else
      return false;
    value = (value << 4) | digit;
  }
  if (!value)
    return false;
  *key = value;
  return true;
}

/* What the opener can ask for of a Metal texture (WMTTextureInfo's fields, copied by the caller;
   usage and storage are the creator's to choose and aren't compared). */
struct Desc {
  uint32_t type = 0;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t array_length = 0;
  uint32_t mip_levels = 0;
  uint32_t samples = 0;
};

/* "" when they agree, else what differs: "width 1280 vs 640, format 80 vs 70" (asked vs shared) */
inline std::string
DescriptionDiff(const Desc &asked, const Desc &found) {
  std::string out;
  auto field = [&](const char *name, uint32_t a, uint32_t f) {
    if (a == f)
      return;
    if (!out.empty())
      out += ", ";
    out += std::string(name) + " " + std::to_string(a) + " vs " + std::to_string(f);
  };
  field("type", asked.type, found.type);
  field("format", asked.format, found.format);
  field("width", asked.width, found.width);
  field("height", asked.height, found.height);
  field("depth", asked.depth, found.depth);
  field("array", asked.array_length, found.array_length);
  field("levels", asked.mip_levels, found.mip_levels);
  field("samples", asked.samples, found.samples);
  return out;
}

inline std::string
Hex(uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
  return text;
}

/* "[d3d11] shared texture <w>x<h> <format> misc 0x<flags> bind 0x<flags> -> <outcome>" */
inline std::string
TextureLine(uint32_t width, uint32_t height, const std::string &format, uint32_t misc, uint32_t bind,
            const std::string &outcome) {
  return "[d3d11] shared texture " + std::to_string(width) + "x" + std::to_string(height) + " " + format +
         " misc " + Hex(misc) + " bind " + Hex(bind) + " -> " + outcome;
}

/* "[d3d11] open shared <handle or name>: found|missing|mismatch (<what differs>)"; `what` is
   "texture" or "fence" ("open shared fence 0x..."). */
inline std::string
OpenLine(const char *what, const std::string &handle, const char *outcome, const std::string &detail) {
  std::string line = std::string("[d3d11] open shared ") + (std::strcmp(what, "fence") == 0 ? "fence " : "") +
                     handle + ": " + outcome;
  if (!detail.empty())
    line += " (" + detail + ")";
  return line;
}

/* "[d3d11] D3DKMT<Call> -> 0x<status>[; <detail>]" */
inline std::string
KmtLine(const char *call, uint32_t status, const std::string &detail) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08x", status);
  std::string line = std::string("[d3d11] ") + call + " -> " + text;
  if (!detail.empty())
    line += "; " + detail;
  return line;
}

constexpr uint64_t kRepeatPeriodMs = 10'000;
constexpr unsigned kMaxKinds = 64;

/* Each kind's first line at once; later lines of a kind are counted and the last one kept. 10 s
   after the first repeat not yet reported, one line per kind that came again:
     [d3d11] <kind>: <n> more (last: <its line without "[d3d11] ">)
   Past kMaxKinds kinds a new kind is dropped. Thread-safe. */
class ShareLog {
public:
  struct Out {
    bool warn;
    std::string line;
  };

  /* The lines to write now: the line itself when it is its kind's first, and the repeat lines
     when they are due. */
  std::vector<Out>
  note(const std::string &kind, bool warn, const std::string &line, uint64_t now_ms) {
    std::vector<Out> out;
    std::lock_guard<std::mutex> lock(mutex_);
    Kind *k = find(kind);
    if (!k) {
      if (kinds_.size() < kMaxKinds) {
        kinds_.push_back(Kind{kind, warn, 0, std::string()});
        out.push_back(Out{warn, line});
      }
    } else {
      k->repeats++;
      k->last = line;
      if (!due_ms_.load(std::memory_order_relaxed))
        due_ms_.store(now_ms + kRepeatPeriodMs, std::memory_order_relaxed);
    }
    takeDue(now_ms, out);
    return out;
  }

  /* The repeat lines, when they are due. */
  std::vector<Out>
  tick(uint64_t now_ms) {
    std::vector<Out> out;
    std::lock_guard<std::mutex> lock(mutex_);
    takeDue(now_ms, out);
    return out;
  }

  /* A repeat waits to be reported (unlocked, for Present's quick test). */
  bool
  pending() const {
    return due_ms_.load(std::memory_order_relaxed) != 0;
  }

private:
  struct Kind {
    std::string name;
    bool warn;
    uint32_t repeats;
    std::string last;
  };

  Kind *
  find(const std::string &kind) {
    for (auto &k : kinds_)
      if (k.name == kind)
        return &k;
    return nullptr;
  }

  void
  takeDue(uint64_t now_ms, std::vector<Out> &out) {
    const uint64_t due = due_ms_.load(std::memory_order_relaxed);
    if (!due || now_ms < due)
      return;
    for (auto &k : kinds_) {
      if (!k.repeats)
        continue;
      std::string last = k.last.rfind("[d3d11] ", 0) == 0 ? k.last.substr(8) : k.last;
      out.push_back(Out{k.warn, "[d3d11] " + k.name + ": " + std::to_string(k.repeats) + " more (last: " + last + ")"});
      k.repeats = 0;
      k.last.clear();
    }
    due_ms_.store(0, std::memory_order_relaxed);
  }

  std::mutex mutex_;
  std::vector<Kind> kinds_;
  std::atomic<uint64_t> due_ms_{0}; // 0: no repeat waiting (written under the lock)
};

} // namespace dxmt::share

/* d3d11_share.cpp: the process's ShareLog, written through the logger, and the description of a
   WMTTextureInfo. Declared here, defined there (the host test uses only the pure parts above). */
struct WMTTextureInfo;

namespace dxmt::share {
void Note(const std::string &kind, bool warn, const std::string &line);
/* from every Present: the repeat lines, when due (one relaxed load when none waits) */
void Tick();
Desc DescOf(const WMTTextureInfo &info);
} // namespace dxmt::share
