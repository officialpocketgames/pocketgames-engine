// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 4: no log message may overflow Wine's debug buffer.
// The 32-bit ntdll holds about 1020 bytes per debug message; a longer one makes
// it log "debug buffer overflow" and raise STATUS_BUFFER_OVERFLOW, which ends
// the program. The logger (log.cpp) builds every line it hands to Wine, for
// every level, with FormatLogLine, so none is longer than kLogMessageMaxBytes.
// Pure, without Windows types, so the host test (tests/test_log_cap.cpp)
// builds it as it is.

#pragma once

#include <cstddef>
#include <string>

namespace dxmt {

/* The longest message the logger passes to Wine's debug output, newline
   included. */
constexpr size_t kLogMessageMaxBytes = 900;

/* "…" in UTF-8. */
constexpr const char kLogEllipsis[] = "\xE2\x80\xA6";
constexpr size_t kLogEllipsisBytes = sizeof(kLogEllipsis) - 1;

/* `text` cut to at most `max_bytes`: unchanged if it fits; otherwise cut at a
   UTF-8 character boundary and ended with "…". */
inline std::string
CapLogText(const std::string &text, size_t max_bytes) {
  if (text.size() <= max_bytes)
    return text;
  if (max_bytes < kLogEllipsisBytes)
    return std::string();
  size_t cut = max_bytes - kLogEllipsisBytes;
  // A continuation byte (10xxxxxx) at the cut belongs to the character before
  // it: move the cut back to that character's first byte, dropping it whole.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
    cut--;
  return text.substr(0, cut) + kLogEllipsis;
}

/* The message for one line of a log call: `prefix` and `line`, cut if needed,
   then the newline that ends Wine's message (kept, so the next message never
   joins a cut one in Wine's per-thread buffer). At most kLogMessageMaxBytes. */
inline std::string
FormatLogLine(const char *prefix, const std::string &line) {
  return CapLogText(std::string(prefix) + line, kLogMessageMaxBytes - 1) + "\n";
}

} // namespace dxmt
