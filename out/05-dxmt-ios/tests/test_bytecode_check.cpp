// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 3 (item 3): shader bytecode checked before use (dxmt/src/d3d11/d3d11_bytecode_check.hpp).
// For each of the eight creates that take bytecode, the three refusals (null, short, not DXBC)
// with their line once per method, and a DXBC blob let through.
//   c++ -std=c++20 -I../dxmt/src/d3d11 test_bytecode_check.cpp

#include "test_common.hpp"

#include "d3d11_bytecode_check.hpp"

#include <string>
#include <vector>

using namespace dxmt;

namespace {

void
TestCheck() {
  unsigned char dxbc[64] = {'D', 'X', 'B', 'C'};
  unsigned char other[64] = {'D', 'X', 'B', 'D'};
  unsigned char spirv[64] = {0x03, 0x02, 0x23, 0x07};
  CHECK(CheckBytecode(nullptr, 64) == BytecodeProblem::Null);
  CHECK(CheckBytecode(nullptr, 0) == BytecodeProblem::Null);
  CHECK(CheckBytecode(dxbc, 0) == BytecodeProblem::Short);
  CHECK(CheckBytecode(dxbc, 4) == BytecodeProblem::Short);
  CHECK(CheckBytecode(dxbc, 31) == BytecodeProblem::Short);
  CHECK(CheckBytecode(dxbc, 32) == BytecodeProblem::None);
  CHECK(CheckBytecode(dxbc, 64) == BytecodeProblem::None);
  CHECK(CheckBytecode(other, 64) == BytecodeProblem::NotDxbc);
  CHECK(CheckBytecode(spirv, 64) == BytecodeProblem::NotDxbc);
  CHECK(CheckBytecode(other, 8) == BytecodeProblem::Short); // short is said before the magic
  CHECK(std::string(BytecodeProblemName(BytecodeProblem::NotDxbc)) == "not DXBC");
}

void
TestEachMethod() {
  unsigned char dxbc[48] = {'D', 'X', 'B', 'C'};
  unsigned char not_dxbc[48] = {'R', 'D', 'E', 'F'};
  const char *expected_names[] = {
      "CreateInputLayout",    "CreateVertexShader",
      "CreatePixelShader",    "CreateGeometryShader",
      "CreateGeometryShaderWithStreamOutput", "CreateHullShader",
      "CreateDomainShader",   "CreateComputeShader",
  };
  BytecodeRefusalLog log;
  std::vector<std::string> lines;
  auto write = [&](const std::string &line) { lines.push_back(line); };
  for (unsigned m = 0; m < unsigned(BytecodeMethod::Count); m++) {
    BytecodeMethod method = BytecodeMethod(m);
    CHECK(std::string(BytecodeMethodName(method)) == expected_names[m]);
    size_t before = lines.size();
    // the three cases: each refused (E_INVALIDARG); the line for the first only
    CHECK(RefuseBytecode(log, method, nullptr, 48, write));
    CHECK(RefuseBytecode(log, method, dxbc, 16, write));
    CHECK(RefuseBytecode(log, method, not_dxbc, 48, write));
    CHECK_EQ(lines.size(), before + 1);
    if (lines.size() == before + 1)
      CHECK(lines.back() == std::string("[d3d11] ") + expected_names[m] + ": bytecode refused (null)");
    // a DXBC blob goes on to be parsed, with no line
    CHECK(!RefuseBytecode(log, method, dxbc, 48, write));
    CHECK_EQ(lines.size(), before + 1);
  }
  CHECK_EQ(lines.size(), size_t(BytecodeMethod::Count));

  // each case's own line, when it is a method's first refusal
  BytecodeRefusalLog fresh;
  std::vector<std::string> first;
  auto keep = [&](const std::string &line) { first.push_back(line); };
  RefuseBytecode(fresh, BytecodeMethod::CreatePixelShader, dxbc, 31, keep);
  RefuseBytecode(fresh, BytecodeMethod::CreateComputeShader, not_dxbc, 48, keep);
  RefuseBytecode(fresh, BytecodeMethod::CreateInputLayout, nullptr, 0, keep);
  CHECK_EQ(first.size(), 3u);
  if (first.size() == 3) {
    CHECK(first[0] == "[d3d11] CreatePixelShader: bytecode refused (short)");
    CHECK(first[1] == "[d3d11] CreateComputeShader: bytecode refused (not DXBC)");
    CHECK(first[2] == "[d3d11] CreateInputLayout: bytecode refused (null)");
  }
}

} // namespace

int
main() {
  TestCheck();
  TestEachMethod();
  return TestSummary("test_bytecode_check");
}
