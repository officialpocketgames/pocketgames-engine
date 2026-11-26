// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 3 (item 3; spec 05's files, both bitnesses): shader bytecode is checked before it
// is hashed or parsed. A 32-bit Direct3D 10 game read null in d3d11.dll right after two of its
// run-time shader compiles failed: it passed the result, a null or short blob, to a create call.
// The shader and input layout creates return E_INVALIDARG when the bytecode pointer is null, the
// length is under 32 (a DXBC header's size), or the blob doesn't start with "DXBC", and log once
// per method:
//   [d3d11] <method>: bytecode refused (<null|short|not DXBC>)
// The Direct3D 10 creates forward to these, so they are covered too. Plain C++, so the host test
// builds it as it is.
#pragma once

#include <atomic>
#include <cstddef>
#include <string>

namespace dxmt {

enum class BytecodeProblem { None, Null, Short, NotDxbc };

constexpr size_t kMinBytecodeLength = 32;

inline BytecodeProblem
CheckBytecode(const void *code, size_t length) {
  if (!code)
    return BytecodeProblem::Null;
  if (length < kMinBytecodeLength)
    return BytecodeProblem::Short;
  const unsigned char *bytes = static_cast<const unsigned char *>(code);
  if (bytes[0] != 'D' || bytes[1] != 'X' || bytes[2] != 'B' || bytes[3] != 'C')
    return BytecodeProblem::NotDxbc;
  return BytecodeProblem::None;
}

inline const char *
BytecodeProblemName(BytecodeProblem problem) {
  switch (problem) {
  case BytecodeProblem::Null:
    return "null";
  case BytecodeProblem::Short:
    return "short";
  case BytecodeProblem::NotDxbc:
    return "not DXBC";
  case BytecodeProblem::None:
    break;
  }
  return "fine";
}

/* the creates that take bytecode */
enum class BytecodeMethod {
  CreateInputLayout,
  CreateVertexShader,
  CreatePixelShader,
  CreateGeometryShader,
  CreateGeometryShaderWithStreamOutput,
  CreateHullShader,
  CreateDomainShader,
  CreateComputeShader,
  Count
};

inline const char *
BytecodeMethodName(BytecodeMethod method) {
  switch (method) {
  case BytecodeMethod::CreateInputLayout:
    return "CreateInputLayout";
  case BytecodeMethod::CreateVertexShader:
    return "CreateVertexShader";
  case BytecodeMethod::CreatePixelShader:
    return "CreatePixelShader";
  case BytecodeMethod::CreateGeometryShader:
    return "CreateGeometryShader";
  case BytecodeMethod::CreateGeometryShaderWithStreamOutput:
    return "CreateGeometryShaderWithStreamOutput";
  case BytecodeMethod::CreateHullShader:
    return "CreateHullShader";
  case BytecodeMethod::CreateDomainShader:
    return "CreateDomainShader";
  case BytecodeMethod::CreateComputeShader:
    return "CreateComputeShader";
  case BytecodeMethod::Count:
    break;
  }
  return "?";
}

inline std::string
BytecodeRefusedLine(BytecodeMethod method, BytecodeProblem problem) {
  return std::string("[d3d11] ") + BytecodeMethodName(method) + ": bytecode refused (" + BytecodeProblemName(problem) +
         ")";
}

/* Which methods have logged their line */
class BytecodeRefusalLog {
public:
  /* true the first time `method` refuses */
  bool
  take(BytecodeMethod method) {
    return !logged_[unsigned(method)].exchange(true, std::memory_order_relaxed);
  }

private:
  std::atomic<bool> logged_[unsigned(BytecodeMethod::Count)] = {};
};

/* Whether `method` refuses this bytecode (its create then returns E_INVALIDARG); `write` gets the
   line the first time each method refuses */
template <typename Write>
bool
RefuseBytecode(
    BytecodeRefusalLog &log, BytecodeMethod method, const void *code, size_t length, Write &&write
) {
  BytecodeProblem problem = CheckBytecode(code, length);
  if (problem == BytecodeProblem::None)
    return false;
  if (log.take(method))
    write(BytecodeRefusedLine(method, problem));
  return true;
}

} // namespace dxmt
