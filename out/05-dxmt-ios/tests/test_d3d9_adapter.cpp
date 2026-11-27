// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 8: the adapter identity Direct3D 9 reports
// (dxmt/src/d3d9/d3d9_adapter_identity.hpp): the NVIDIA default; PG_D3D9_ADAPTER's
// <vendor hex>:<device hex>:<description> parsed; `metal`; malformed values ignored with their
// line; the driver version; the identity line.

#include "test_common.hpp"

#include "d3d9_adapter_identity.hpp"

#include <string>

namespace {

using namespace dxmt::d9adapter;

void
TestDefault() {
  bool malformed = true;
  Identity id = Choose(nullptr, "Apple A17 Pro GPU", &malformed);
  CHECK(!malformed);
  CHECK_EQ(id.vendor, 0x10DEu);
  CHECK_EQ(id.device, 0x0402u);
  CHECK(id.description == "NVIDIA GeForce 8600 GT");
  CHECK(id.source == Source::Default);
  CHECK(Line(id) == "[d3d9] adapter identity 10de:0402 \"NVIDIA GeForce 8600 GT\" (default)");
  // 9.18.13.4144: HighPart 0x00090012, LowPart 0x000D1030
  CHECK_EQ(uint64_t(DriverVersionQuad(id)), 0x00090012000D1030ull);
  // an empty value is unset too
  id = Choose("", "Apple A17 Pro GPU", &malformed);
  CHECK(!malformed && id.source == Source::Default && id.vendor == 0x10DEu);
}

void
TestEnv() {
  bool malformed = true;
  Identity id = Choose("1002:6779:AMD Radeon HD 6450", "Apple GPU", &malformed);
  CHECK(!malformed);
  CHECK_EQ(id.vendor, 0x1002u);
  CHECK_EQ(id.device, 0x6779u);
  CHECK(id.description == "AMD Radeon HD 6450");
  CHECK(id.source == Source::Env);
  CHECK_EQ(DriverVersionQuad(id), INT64_MAX);
  CHECK(Line(id) == "[d3d9] adapter identity 1002:6779 \"AMD Radeon HD 6450\" (env)");
  // 0x prefixes, upper case, short IDs, colons in the description
  id = Choose("0x10DE:0x5e2:NVIDIA: GeForce GTX 260", "Apple GPU", &malformed);
  CHECK(!malformed);
  CHECK_EQ(id.vendor, 0x10DEu);
  CHECK_EQ(id.device, 0x05E2u);
  CHECK(id.description == "NVIDIA: GeForce GTX 260");
  CHECK(Line(id) == "[d3d9] adapter identity 10de:05e2 \"NVIDIA: GeForce GTX 260\" (env)");
  // a description longer than the field keeps 511 bytes
  std::string longer = "8086:0046:" + std::string(600, 'x');
  id = Choose(longer.c_str(), "Apple GPU", &malformed);
  CHECK(!malformed);
  CHECK_EQ(id.description.size(), kMaxDescription);
}

void
TestMetal() {
  bool malformed = true;
  Identity id = Choose("metal", "Apple A17 Pro GPU", &malformed);
  CHECK(!malformed);
  CHECK_EQ(id.vendor, 0x106Bu);
  CHECK_EQ(id.device, 1u);
  CHECK(id.description == "Apple A17 Pro GPU");
  CHECK(id.source == Source::Metal);
  CHECK_EQ(DriverVersionQuad(id), INT64_MAX); // today's newest-driver value
  CHECK(Line(id) == "[d3d9] adapter identity 106b:0001 \"Apple A17 Pro GPU\" (metal)");
  CHECK(Choose("METAL", "Apple GPU", &malformed).source == Source::Metal);
}

void
TestMalformed() {
  const char *bad[] = {
      "nvidia",          // neither form
      "10de:0402",       // no description
      "10de:0402:",      // an empty description
      "10de0402:x",      // one colon
      "10de1:0402:x",    // five digits
      "10dg:0402:x",     // not hex
      ":0402:x",         // no vendor
      "10de::x",         // no device
      "0x:0402:x",       // a prefix alone
      "metals",          // not `metal`
  };
  for (const char *value : bad) {
    bool malformed = false;
    Identity id = Choose(value, "Apple GPU", &malformed);
    CHECK(malformed);
    CHECK(id.source == Source::Default);
    CHECK_EQ(id.vendor, 0x10DEu);
    CHECK_EQ(id.device, 0x0402u);
  }
  CHECK(
      MalformedLine("10de:0402") ==
      "[d3d9] PG_D3D9_ADAPTER \"10de:0402\" is neither <vendor hex>:<device hex>:<description> nor metal; ignored"
  );
}

void
TestHex() {
  uint32_t v = 0;
  CHECK(ParseHex16("0", &v) && v == 0);
  CHECK(ParseHex16("ffff", &v) && v == 0xFFFF);
  CHECK(ParseHex16("0XaB", &v) && v == 0xAB);
  CHECK(!ParseHex16("", &v));
  CHECK(!ParseHex16("12345", &v));
  CHECK(!ParseHex16("-1", &v));
}

} // namespace

int
main() {
  TestDefault();
  TestEnv();
  TestMetal();
  TestMalformed();
  TestHex();
  return TestSummary("test_d3d9_adapter");
}
