#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 05 clarification 13 (the six upstream DXMT fixes), host side. The DXBC converter needs
# LLVM 15, which the host tests don't have, so this checks the converter's source for the
# shape of each fix instead of converting shaders (TESTS.md says what only a device build
# shows):
#   - typed buffers: TexelAccessInRange compares the element index against the view's element
#     count (DecodeTextureBufferElement), clamps it to 0, and only then adds the view's offset;
#     no other place adds that offset; ld and the typed UAV load select 0 when out of range,
#     the typed UAV store branches, and both typed atomics go through EmitAtomicIfInRange;
#   - every sample, gather and LOD-query coordinate (and sample_d's derivatives) goes through
#     getSampleOperand, which selects 0 for NaN components when the option is set; the NaN
#     test is the bit test; every DXBC converter passes the common argument's flags to the
#     AIR builder; Direct3D 11 sets the flag unless d3d11.sampleNaNToZero is False, and the
#     32-bit argument chain copies it;
#   - udiv: quotient and remainder select all ones on a 0 divisor, with the divisor made 1;
#   - sample_l adds the sampler's bias to its LOD;
#   - interpolate-at-offset takes float2, and mul_hi has its name;
#   - clarification 16: DXBC min and max call the precise air.fmin / air.fmax, every DXBC
#     saturate goes through CreateD3DSaturate (precise air.saturate, then NaN to 0 on the
#     bits), and "fast_" is only added to a name when FastVariant is asked for.
# Usage: check-shader-fixes.sh [dxmt/src directory]. POSIX sh, awk and grep.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=${1:-"$here/../dxmt/src"}
base="$src/airconv/nt/dxbc_converter_base.cpp"
builder="$src/airconv/nt/air_builder.cpp"
builder_h="$src/airconv/nt/air_builder.hpp"

failures=0
fail() { echo "FAIL check-shader-fixes: $1"; failures=$((failures + 1)); }

# The text of the function whose definition line starts with the literal text $2 in $1, up to
# the first line that is a lone "}" (this tree's style), carriage returns removed. Exits the
# check when there is no such function.
body() {
  LC_ALL=C awk -v start="$2" '
    { sub(/\r$/, "") }
    !inside && index($0, start) == 1 { inside = 1 }
    inside { print }
    inside && /^}/ { exit }
    END { if (!inside) { print "FAIL check-shader-fixes: no function " start " in " FILENAME > "/dev/stderr"; exit 1 } }
  ' "$1"
}

# has <name> <text> <fixed string>...: each string occurs in the text.
has() {
  name=$1
  text=$2
  shift 2
  for needle in "$@"; do
    printf '%s\n' "$text" | grep -qF -- "$needle" || fail "$name: no \"$needle\""
  done
}

count() { printf '%s\n' "$1" | grep -cF -- "$2" || true; }

for f in "$base" "$builder" "$builder_h" "$src/airconv/dxbc_converter.hpp" "$src/d3d11/d3d11_shader.cpp" \
  "$src/winemetal/unix/winemetal_unix.c"; do
  [ -f "$f" ] || { echo "check-shader-fixes: $f missing" >&2; exit 1; }
done

# --- typed buffers (upstream DXMT 3bcaf16b9) ---
range=$(body "$base" 'Converter::TexelAccessInRange(')
case_text=$(printf '%s\n' "$range" | LC_ALL=C awk '/case Texture::texture_buffer:/ { on = 1 } on { print } on && /return InRange;/ { exit }')
has "TexelAccessInRange texture_buffer" "$case_text" \
  "ir.CreateICmpULT(Address, DecodeTextureBufferElement(Tex.Metadata))" \
  "Address = ir.CreateSelect(InRange, Address, llvm::Constant::getNullValue(Address->getType()));" \
  "Address = ir.CreateAdd(Address, DecodeTextureBufferOffset(Tex.Metadata));"
# the clamp comes before the offset is added
order=$(printf '%s\n' "$case_text" | LC_ALL=C awk '
  /CreateSelect\(InRange, Address/ { s = NR }
  /DecodeTextureBufferOffset/ { o = NR }
  END { print (s && o && s < o) ? "ok" : "bad" }')
[ "$order" = ok ] || fail "TexelAccessInRange texture_buffer: the view offset is added before the clamp"
offsets=$(LC_ALL=C grep -c "DecodeTextureBufferOffset(" "$base" || true)
[ "$offsets" = 1 ] || fail "DecodeTextureBufferOffset is used $offsets times in dxbc_converter_base.cpp, expected once (in TexelAccessInRange)"

ld=$(body "$base" 'Converter::operator()(const InstLoad &')
has "ld" "$ld" "TexelAccessInRange(*Tex, Address, ArrayIndex, LOD, true)" \
  "Value = ir.CreateSelect(InRange, Value, llvm::Constant::getNullValue(Value->getType()));"
uav_load=$(body "$base" 'Converter::operator()(const InstLoadUAVTyped &')
has "typed UAV load" "$uav_load" "TexelAccessInRange(*Tex, Address, ArrayIndex, NoLOD, false)" \
  "Value = ir.CreateSelect(InRange, Value, llvm::Constant::getNullValue(Value->getType()));"
uav_store=$(body "$base" 'Converter::operator()(const InstStoreUAVTyped &')
has "typed UAV store" "$uav_store" "TexelAccessInRange(*Tex, Address, ArrayIndex, NoLOD, false)" \
  "ir.CreateCondBr(InRange, StoreBB, ContinueBB);"
for inst in InstAtomicBinOp InstAtomicImmCmpExchange; do
  atomic=$(body "$base" "Converter::operator()(const $inst &")
  has "$inst" "$atomic" "if (Tex->Logical == Texture::texture_buffer) {" \
    "TexelAccessInRange(*Tex, Address, ArrayIndex, NoLOD, false)" \
    "Value = EmitAtomicIfInRange(ir, InRange, EmitAtomic);"
done
guard=$(body "$base" 'EmitAtomicIfInRange(')
has "EmitAtomicIfInRange" "$guard" "ir.CreateCondBr(InRange, AtomicBB, DoneBB);" \
  "Phi->addIncoming(Result, AtomicEndBB);" \
  "Phi->addIncoming(llvm::Constant::getNullValue(Result->getType()), EntryBB);"

# --- NaN-to-0 sample coordinates (upstream DXMT f48f4c707) ---
# every coordinate type pushed is followed by its operand through getSampleOperand
unguarded=$(LC_ALL=C awk '
  { sub(/\r$/, "") }
  want { if ($0 !~ /Ops\.push_back\(getSampleOperand\((Coord|DerivX|DerivY)\)\);/) print FILENAME ":" NR ": " $0; want = 0 }
  /Tys\.push_back\(getTextureSampleCoordType\(Texture\)\);/ { want = 1 }
' "$builder")
[ -z "$unguarded" ] || fail "a sample coordinate skips getSampleOperand: $unguarded"
coords=$(LC_ALL=C grep -c "Ops.push_back(getSampleOperand(Coord));" "$builder" || true)
[ "$coords" = 6 ] || fail "$coords coordinates through getSampleOperand, expected 6 (sample, sample_c, sample_d, gather, gather_c, lod)"
grad=$(body "$builder" 'AIRBuilder::CreateSampleGrad(')
has "sample_d" "$grad" "Ops.push_back(getSampleOperand(DerivX));" "Ops.push_back(getSampleOperand(DerivY));"
raw=$(LC_ALL=C grep -cE "Ops\.push_back\((Coord|DerivX|DerivY)\);" "$builder" || true)
[ "$raw" = 0 ] || fail "$raw raw coordinate operands left in air_builder.cpp"
isnan=$(body "$builder" 'AIRBuilder::CreateIsNaN(')
has "CreateIsNaN" "$isnan" "getInt(0x7fffffffu)" "getInt(0x7f800000u)" \
  "builder.CreateICmpUGT(builder.CreateAnd(builder.CreateBitCast(Operand, CastType), AbsBits), InfBits)"
header=$(tr -d '\r' < "$builder_h")
has "air_builder.hpp" "$header" "return options.sampleNaNToZero ? getNaNToZero(Operand) : Operand;" \
  "return builder.CreateSelect(CreateIsNaN(Operand), Constant::getNullValue(Operand->getType()), Operand);"
for f in "$src"/airconv/dxbc_converter.cpp "$src"/airconv/dxbc_converter_gs.cpp "$src"/airconv/dxbc_converter_ts.cpp; do
  plain=$(LC_ALL=C grep -n "AIRBuilder air(" "$f" | grep -vF "AIRBuilder air(air_builder_options(sm50_common), builder, nulldbg);" || true)
  [ -z "$plain" ] || fail "$(basename "$f"): AIR builder without the flags: $plain"
done
total=$(cat "$src"/airconv/dxbc_converter.cpp "$src"/airconv/dxbc_converter_gs.cpp "$src"/airconv/dxbc_converter_ts.cpp |
  LC_ALL=C grep -c "air_builder_options(sm50_common)" || true)
[ "$total" = 7 ] || fail "$total DXBC converters pass the flags, expected 7 (ps, cs, vs, gs, vs-for-gs, hull, domain)"
has "dxbc_converter.hpp" "$(tr -d '\r' < "$src/airconv/dxbc_converter.hpp")" \
  "options.sampleNaNToZero = sm50_common && (sm50_common->flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO);"
has "d3d11_shader.cpp" "$(tr -d '\r' < "$src/d3d11/d3d11_shader.cpp")" \
  'getOption<bool>("d3d11.sampleNaNToZero", true) ? SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO : 0u' \
  "sm50_common.flags = GetGlobalShaderFlags();" \
  "variant_digest_(CacheKeyWithFlags(variant_digest, GetGlobalShaderFlags()))"
has "winemetal_unix.c" "$(tr -d '\r' < "$src/winemetal/unix/winemetal_unix.c")" "data->flags = src->flags;"

# --- udiv by 0 (upstream DXMT 46b1146de) ---
binop=$(body "$base" 'Converter::operator()(const InstIntegerBinaryOpWithTwoDst &')
udiv=$(printf '%s\n' "$binop" | LC_ALL=C awk '/case IntegerBinaryOpWithTwoDst::UDiv:/ { on = 1 } on { print } on && /^    break;/ { exit }')
has "udiv" "$udiv" "auto IsZero = ir.CreateIsNull(Den);" \
  "auto SafeDen = ir.CreateSelect(IsZero, llvm::ConstantInt::get(Den->getType(), 1), Den);" \
  "return ir.CreateSelect(IsZero, llvm::ConstantInt::getAllOnesValue(Den->getType()), Result);" \
  "StoreOperand(bin.dst_hi, DivideOrAllOnes(Src0Hi, Src1Hi, false));" \
  "StoreOperand(bin.dst_low, DivideOrAllOnes(Src0Lo, Src1Lo, true));"
[ "$(count "$udiv" "ir.CreateUDiv(Num, SafeDen)")" = 1 ] || fail "udiv: the quotient doesn't divide by the safe divisor"
[ "$(count "$udiv" "ir.CreateURem(Num, SafeDen)")" = 1 ] || fail "udiv: the remainder doesn't divide by the safe divisor"

# --- sample_l bias (upstream DXMT eec0b65a5) ---
sample_l=$(body "$base" 'Converter::operator()(const InstSampleLOD &')
has "sample_l" "$sample_l" \
  "llvm::Value *LOD = ir.CreateFAdd(LoadOperand(sample.src_lod, kMaskComponentX), Sampler->Bias);" \
  "sample_level{LOD}"

# --- ae9126982's builder changes ---
interp=$(body "$builder" 'AIRBuilder::CreateInterpolateAtOffset(')
has "interpolate at offset" "$interp" "{Interpoant->getType(), getFloatTy(2)}"
intbin=$(body "$builder" 'AIRBuilder::CreateIntBinOp(')
has "integer binary operations" "$intbin" '"max",' '"min",' '"mul_hi",'

# --- spec 05 clarification 16: DXBC min, max and saturate are Metal's precise forms ---
# "fast_" is only added for FastVariant, in both builders
for fn in CreateFPUnOp CreateFPBinOp; do
  fp=$(body "$builder" "AIRBuilder::$fn(")
  prefix=$(printf '%s\n' "$fp" | LC_ALL=C awk '
    /if \(FastVariant\)/ { guard = NR }
    /FnName \+= "fast_";/ { print (guard && NR == guard + 1) ? "ok" : "bad"; exit }')
  [ "$prefix" = ok ] || fail "$fn: \"fast_\" is not guarded by FastVariant"
done
floatbin=$(body "$base" 'Converter::operator()(const InstFloatBinaryOp &')
has "DXBC min and max" "$floatbin" \
  "Result = air.CreateFPBinOp(AIRBuilder::fmin, LHS, RHS, /*FastVariant=*/false);" \
  "Result = air.CreateFPBinOp(AIRBuilder::fmax, LHS, RHS, /*FastVariant=*/false);"
fastminmax=$(printf '%s\n' "$floatbin" | LC_ALL=C grep -E "AIRBuilder::fm(in|ax), LHS, RHS\)" || true)
[ -z "$fastminmax" ] || fail "DXBC min or max still fast: $fastminmax"
# every saturate the DXBC converters emit goes through CreateD3DSaturate
for f in "$base" "$src/airconv/nt/dxbc_converter_base.hpp" "$src"/airconv/dxbc_converter.cpp \
  "$src"/airconv/dxbc_converter_gs.cpp "$src"/airconv/dxbc_converter_ts.cpp "$src"/airconv/dxbc_converter_basicblock.cpp; do
  raw_sat=$(LC_ALL=C grep -n "AIRBuilder::saturate" "$f" || true)
  [ -z "$raw_sat" ] || fail "$(basename "$f"): saturate not through CreateD3DSaturate: $raw_sat"
done
d3d_sat=$(tr -d '\r' < "$src/airconv/nt/dxbc_converter_base.hpp" | LC_ALL=C grep -c "air.CreateD3DSaturate(" || true)
[ "$d3d_sat" = 2 ] || fail "StoreOperand and StoreOperandVec4 saturate through CreateD3DSaturate: $d3d_sat, expected 2"
has "CreateD3DSaturate" "$header" \
  "return getNaNToZero(CreateFPUnOp(saturate, Operand, /*FastVariant=*/false));"

if [ "$failures" -ne 0 ]; then
  echo "check-shader-fixes: $failures failure(s)"
  exit 1
fi
echo "check-shader-fixes: all checks passed"
