// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 4 (Part D): host test for the cap on every message the
// logger hands to Wine's debug output (dxmt/src/util/log/log_cap.hpp, used by
// log.cpp for every level): a 2000-byte message is cut to at most 900 bytes and
// ends with "…"; a cut never splits a multi-byte character; short messages are
// unchanged; the newline that ends Wine's message is always kept.
//
//   c++ -std=c++20 -I../dxmt/src/util/log test_log_cap.cpp -o test_log_cap

#include "test_common.hpp"

#include "log_cap.hpp"

#include <string>

using namespace dxmt;

namespace {

/* True when `s` is well-formed UTF-8 (no cut-off character anywhere). */
bool
ValidUtf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0 || i + len > s.size())
      return false;
    for (size_t k = 1; k < len; k++)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
        return false;
    i += len;
  }
  return true;
}

bool
EndsWith(const std::string &s, const std::string &end) {
  return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

void
TestAscii() {
  const std::string ellipsis = "\xE2\x80\xA6";
  // A 2000-byte message: at most 900 bytes, ending with "…".
  const std::string message(2000, 'x');
  const std::string capped = CapLogText(message, kLogMessageMaxBytes);
  CHECK(capped.size() <= 900);
  CHECK_EQ(capped.size(), 900); // ASCII: cut exactly at the limit
  CHECK(EndsWith(capped, ellipsis));
  CHECK_EQ(capped.find_first_not_of('x'), 897);

  // What the logger passes to Wine: prefix and text, cut, then the newline.
  const std::string line = FormatLogLine("info:  ", message);
  CHECK(line.size() <= 900);
  CHECK(EndsWith(line, ellipsis + "\n"));
  CHECK_EQ(line.find("info:  "), 0);

  // Short messages are passed whole.
  CHECK(CapLogText("d3d9 calls: 61234/s", 900) == "d3d9 calls: 61234/s");
  CHECK(FormatLogLine("warn:  ", "short") == "warn:  short\n");
  // Exactly at the limit: unchanged; one byte over: cut.
  const std::string exact(899 - 7, 'y');
  CHECK_EQ(FormatLogLine("err:   ", exact).size(), 900);
  CHECK(!EndsWith(FormatLogLine("err:   ", exact), ellipsis + "\n"));
  CHECK(EndsWith(FormatLogLine("err:   ", exact + "z"), ellipsis + "\n"));
  CHECK(FormatLogLine("err:   ", exact + "z").size() <= 900);
  // A limit too small for the ellipsis gives nothing rather than a broken one.
  CHECK(CapLogText("abcdef", 2).empty());
}

void
TestMultiByte() {
  // Two-, three- and four-byte characters at every offset of the cut: the
  // result is valid UTF-8, at most the limit, and ends with "…".
  const std::string chars[] = {"\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x8E\xAE"}; // é € 🎮
  for (const auto &ch : chars) {
    for (size_t pad = 0; pad < 4; pad++) {
      std::string message(pad, 'a');
      while (message.size() < 2000)
        message += ch;
      const std::string capped = CapLogText(message, kLogMessageMaxBytes);
      CHECK(capped.size() <= 900);
      CHECK(capped.size() >= 900 - 3 - 3); // at most one character dropped beyond the ellipsis
      CHECK(ValidUtf8(capped));
      CHECK(EndsWith(capped, "\xE2\x80\xA6"));
      const std::string line = FormatLogLine("debug: ", message);
      CHECK(line.size() <= 900);
      CHECK(ValidUtf8(line));
    }
  }
}

} // namespace

int
main() {
  TestAscii();
  TestMultiByte();
  return TestSummary("test_log_cap");
}
