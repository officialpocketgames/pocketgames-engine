// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The pattern ANGLE's Direct3D 11 back end writes for GLSL ES 3.00 array and shadow-array
// samplers (written for this test, not taken from any program): one array of textures and one
// of samplers per texture kind, a static const index per GLSL sampler, and helpers that turn the
// index into the kind's array index with const locals. Microsoft's compiler folds every index;
// vkd3d 1.19 moved those const locals to program start, before the helper's argument exists,
// and failed with E5022 ("must be determinable at compile time").
// Expected: t0/s0 once, t1/s1 (the layered texture) for the four layers, t2/s2 (the decals),
// t3/s3 compared (the shadow map) in the loop, and no other register.

static const uint _plain = 0;
uniform Texture2D<float4> textures2D[1] : register(t0);
uniform SamplerState samplers2D[1] : register(s0);
static const uint _layers = 1;
static const uint _decals = 2;
static const uint textureIndexOffset2DArray = 1;
static const uint samplerIndexOffset2DArray = 1;
uniform Texture2DArray<float4> textures2DArray[2] : register(t1);
uniform SamplerState samplers2DArray[2] : register(s1);
static const uint _shadow = 3;
static const uint textureIndexOffset2DArray_comparison = 3;
static const uint samplerIndexOffset2DArray_comparison = 3;
uniform Texture2DArray textures2DArray_comparison[1] : register(t3);
uniform SamplerComparisonState samplers2DArray_comparison[1] : register(s3);

float4 gl_texture2D(uint samplerIndex, float2 t, float bias)
{
    return textures2D[samplerIndex].SampleBias(samplers2D[samplerIndex], float2(t.x, t.y), bias);
}

float4 gl_texture2DArray(uint samplerIndex, float3 t, float bias)
{
    const uint textureIndex = samplerIndex - textureIndexOffset2DArray;
    const uint samplerArrayIndex = samplerIndex - samplerIndexOffset2DArray;
    return textures2DArray[textureIndex].SampleBias(samplers2DArray[samplerArrayIndex], float3(t.x, t.y, t.z), bias);
}

float gl_texture2DArray_comparison(uint samplerIndex, float4 t)
{
    const uint textureIndex = samplerIndex - textureIndexOffset2DArray_comparison;
    const uint samplerArrayIndex = samplerIndex - samplerIndexOffset2DArray_comparison;
    return textures2DArray_comparison[textureIndex].SampleCmp(samplers2DArray_comparison[samplerArrayIndex], float3(t.x, t.y, t.z), t.w);
}

static float3 _uv = {0, 0, 0};

float3 f_layer(in int _i)
{
    return gl_texture2DArray(_layers, float3(_uv.xy, _i), 0.0).xyz;
}

float f_tap(in float2 _offset, in int _cascade, in float _z)
{
    return gl_texture2DArray_comparison(_shadow, float4(_uv.xy + _offset, _cascade, _z));
}

float4 main(float3 uv : TEXCOORD0) : SV_Target
{
    _uv = uv;
    float3 layers = f_layer(0) + f_layer(1) + f_layer(2) + f_layer(3);
    float4 decal = {0, 0, 0, 0};
    if (uv.z >= 0.0)
        decal = gl_texture2DArray(_decals, uv, 0.0);
    float lit = 0.0;
    for (int i = 0; i < 4; ++i)
        lit += f_tap(float2(i, -i) * 0.001, 1, uv.z);
    return float4(layers + decal.xyz, lit) * gl_texture2D(_plain, uv.xy, 0.0);
}
