// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 91: the sharing lines of the process (d3d11_share.hpp's ShareLog) through the
// logger, on the monotonic clock, and a WMTTextureInfo's description for the opener's check.

#include "d3d11_share.hpp"

#include "log/log.hpp"
#include "winemetal.h"

#include <chrono>

namespace dxmt::share {

namespace {

ShareLog &
ProcessLog() {
  static ShareLog log;
  return log;
}

uint64_t
NowMs() {
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  return ms > 0 ? uint64_t(ms) : 1;
}

void
Write(const std::vector<ShareLog::Out> &lines) {
  for (const auto &out : lines) {
    if (out.warn)
      Logger::warn(out.line);
    else
      Logger::info(out.line);
  }
}

} // namespace

void
Note(const std::string &kind, bool warn, const std::string &line) {
  Write(ProcessLog().note(kind, warn, line, NowMs()));
}

void
Tick() {
  ShareLog &log = ProcessLog();
  if (!log.pending())
    return;
  Write(log.tick(NowMs()));
}

Desc
DescOf(const WMTTextureInfo &info) {
  Desc desc;
  desc.type = uint32_t(info.type);
  desc.format = uint32_t(info.pixel_format);
  desc.width = info.width;
  desc.height = info.height;
  desc.depth = info.depth;
  desc.array_length = info.array_length;
  desc.mip_levels = info.mipmap_level_count;
  desc.samples = info.sample_count;
  return desc;
}

} // namespace dxmt::share
