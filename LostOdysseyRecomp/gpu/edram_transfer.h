#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>

// EDRAM tile-owner transfers (renderer TransferRegion): a colour view of one
// class reads the 32-bit guest words that another class wrote. The pixel
// shader packs each source texel into the guest word and unpacks it the way
// the new class reads it. Both classes live in RGBA16F textures.
//
// kTransferHlsl is that shader's code after its bindings: the caller declares
// `Texture2D<float4> src` and `uint4 xeTransfer` (source class, destination
// class, source/destination scale as float bits). ConstantTransfer is its CPU
// twin for a source that holds one value everywhere (one whole pending clear),
// so the renderer can clear the destination instead of drawing.
namespace gpu::edram_transfer {

inline constexpr const char* kTransferHlsl =
    "float Float7e3To32(uint f10) {\n"
    "  f10 &= 0x3FFu; if (f10 == 0u) return 0.0;\n"
    "  uint mantissa = f10 & 0x7Fu, exponent = f10 >> 7;\n"
    "  if (exponent == 0u) { uint lz = firstbithigh(mantissa); uint shift = 7u - lz;\n"
    "    exponent = uint(int(1) - int(shift)); mantissa = (mantissa << shift) & 0x7Fu; }\n"
    "  return asfloat(((exponent + 124u) << 23) | (mantissa << 16));\n"
    "}\n"
    "uint Float32To7e3(float f) {\n"
    "  if (!(f > 0.0)) return 0u;\n"
    "  uint u = asuint(f);\n"
    "  if (u >= 0x41FF73FFu) return 0x3FFu;\n"
    "  if (u < 0x3E800000u) { uint shift = min(125u - (u >> 23), 24u);\n"
    "    u = (0x800000u | (u & 0x7FFFFFu)) >> shift; }\n"
    "  else { u += 0xC2000000u; }\n"
    "  return ((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16) & 0x3FFu;\n"
    "}\n"
    "uint PackGuest(float4 v, uint cls) {\n"
    "  if (cls == 0u) { uint4 c = uint4(saturate(v) * 255.0 + 0.5);\n"
    "    return c.r | (c.g << 8) | (c.b << 16) | (c.a << 24); }\n"
    "  uint a = uint(saturate(v.a) * 3.0 + 0.5);\n"
    "  uint3 c;\n"
    "  if (cls == 1u) c = uint3(saturate(v.rgb) * 1023.0 + 0.5);\n"
    "  else c = uint3(Float32To7e3(v.r), Float32To7e3(v.g), Float32To7e3(v.b));\n"
    "  return c.r | (c.g << 10) | (c.b << 20) | (a << 30);\n"
    "}\n"
    "float4 UnpackGuest(uint w, uint cls) {\n"
    "  if (cls == 0u) return float4(uint4(w, w >> 8, w >> 16, w >> 24) & 0xFFu) * (1.0 / 255.0);\n"
    "  float alpha = float(w >> 30) * (1.0 / 3.0);\n"
    "  uint3 c = uint3(w, w >> 10, w >> 20) & 0x3FFu;\n"
    "  if (cls == 1u) return float4(float3(c) * (1.0 / 1023.0), alpha);\n"
    "  return float4(Float7e3To32(c.x), Float7e3To32(c.y), Float7e3To32(c.z), alpha);\n"
    "}\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  float4 v = src.Load(int3(int2(pos.x * asfloat(xeTransfer.z), pos.y * asfloat(xeTransfer.w)), 0));\n"
    "  return UnpackGuest(PackGuest(v, xeTransfer.x), xeTransfer.y);\n"
    "}\n";

// Classes the transfer handles (the renderer's kClass8888, kClass2101010, kClass7e3).
inline constexpr uint32_t kLastClass = 2;

// FP32 -> FP16 bits. GPUs differ in how they round shader outputs and clear
// values into FP16 targets (NVIDIA and AMD round shader outputs toward zero).
enum class Round { TowardZero, NearestEven, NearestAway };

inline uint16_t FloatToHalf(float f, Round round)
{
    const uint32_t u = std::bit_cast<uint32_t>(f);
    const uint32_t sign = (u >> 16) & 0x8000u;
    const uint32_t a = u & 0x7FFFFFFFu;
    if (a >= 0x7F800000u) return uint16_t(sign | 0x7C00u | (a > 0x7F800000u ? 0x200u : 0u));
    const int32_t e = int32_t(a >> 23) - 127 + 15;
    if (e >= 31) return uint16_t(sign | (round == Round::TowardZero ? 0x7BFFu : 0x7C00u));
    if (e < -11) return uint16_t(sign);
    uint32_t q, rem, half;
    if (e <= 0) {
        const uint32_t m = (a & 0x7FFFFFu) | 0x800000u, shift = uint32_t(14 - e);
        q = m >> shift; rem = m & ((1u << shift) - 1u); half = 1u << (shift - 1u);
    } else {
        q = (uint32_t(e) << 10) | ((a & 0x7FFFFFu) >> 13); rem = a & 0x1FFFu; half = 0x1000u;
    }
    if ((round == Round::NearestEven && (rem > half || (rem == half && (q & 1u)))) ||
        (round == Round::NearestAway && rem >= half))
        ++q; // may carry into the exponent / infinity
    return uint16_t(sign | q);
}

inline float HalfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (e == 0x1Fu) return std::bit_cast<float>(sign | 0x7F800000u | (m << 13));
    if (e == 0u) {
        if (m == 0u) return std::bit_cast<float>(sign);
        e = 113u; // normalise the denormal
        while (!(m & 0x400u)) { m <<= 1; --e; }
        m &= 0x3FFu;
        return std::bit_cast<float>(sign | (e << 23) | (m << 13));
    }
    return std::bit_cast<float>(sign | ((e + 112u) << 23) | (m << 13));
}

// The shader's helpers, step for step in FP32.
inline float Float7e3To32(uint32_t f10)
{
    f10 &= 0x3FFu;
    if (!f10) return 0.0f;
    uint32_t mantissa = f10 & 0x7Fu, exponent = f10 >> 7;
    if (!exponent) {
        const uint32_t shift = 7u - uint32_t(std::bit_width(mantissa) - 1);
        exponent = uint32_t(1 - int32_t(shift));
        mantissa = (mantissa << shift) & 0x7Fu;
    }
    return std::bit_cast<float>(((exponent + 124u) << 23) | (mantissa << 16));
}

inline uint32_t Float32To7e3(float f)
{
    if (!(f > 0.0f)) return 0;
    uint32_t u = std::bit_cast<uint32_t>(f);
    if (u >= 0x41FF73FFu) return 0x3FFu;
    if (u < 0x3E800000u) {
        const uint32_t shift = std::min(125u - (u >> 23), 24u);
        u = (0x800000u | (u & 0x7FFFFFu)) >> shift;
    } else
        u += 0xC2000000u;
    return ((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16) & 0x3FFu;
}

// saturate(x) * scale + 0.5 to uint; fused: as one multiply-add, which a GPU compiler may emit.
inline uint32_t Quantize(float x, float scale, bool fused)
{
    const float s = std::clamp(x, 0.0f, 1.0f);
    const float product = s * scale; // separate statements: no contraction here
    return uint32_t(fused ? std::fma(s, scale, 0.5f) : product + 0.5f);
}

inline uint32_t PackGuest(const float v[4], uint32_t cls, bool fused)
{
    if (cls == 0) {
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i) w |= Quantize(v[i], 255.0f, fused) << (8 * i);
        return w;
    }
    const uint32_t a = Quantize(v[3], 3.0f, fused);
    uint32_t c[3];
    for (int i = 0; i < 3; ++i) c[i] = cls == 1 ? Quantize(v[i], 1023.0f, fused) : Float32To7e3(v[i]);
    return c[0] | (c[1] << 10) | (c[2] << 20) | (a << 30);
}

inline void UnpackGuest(uint32_t w, uint32_t cls, float out[4])
{
    if (cls == 0) {
        for (int i = 0; i < 4; ++i) out[i] = float((w >> (8 * i)) & 0xFFu) * float(1.0 / 255.0);
        return;
    }
    out[3] = float(w >> 30) * float(1.0 / 3.0);
    for (int i = 0; i < 3; ++i) {
        const uint32_t c = (w >> (10 * i)) & 0x3FFu;
        out[i] = cls == 1 ? float(c) * float(1.0 / 1023.0) : Float7e3To32(c);
    }
}

// What the transfer writes when every source texel was cleared to `value`:
// FP16-exact floats for a clear of the destination. False (draw the transfer
// instead) for classes the shader does not handle, for non-finite values, and
// whenever the result could depend on the GPU: on how it rounded the clear into
// the FP16 source or the shader's output into the FP16 destination (toward zero,
// to nearest even or away), or on whether it fused the shader's multiply-add.
// The renderer's EDRAM clears (zero and all-ones words, 7e3 values) qualify.
inline bool ConstantTransfer(const float value[4], uint32_t srcClass, uint32_t dstClass, float out[4])
{
    if (srcClass > kLastClass || dstClass > kLastClass) return false;
    constexpr Round kModes[] = {Round::TowardZero, Round::NearestEven, Round::NearestAway};
    uint32_t word = 0;
    for (size_t m = 0; m < std::size(kModes); ++m) {
        float texel[4];
        for (int i = 0; i < 4; ++i) {
            if (!std::isfinite(value[i])) return false;
            texel[i] = HalfToFloat(FloatToHalf(value[i], kModes[m]));
        }
        for (bool fused : {false, true}) {
            const uint32_t w = PackGuest(texel, srcClass, fused);
            if (m == 0 && !fused) word = w;
            else if (w != word) return false;
        }
    }
    float unpacked[4];
    UnpackGuest(word, dstClass, unpacked);
    for (int i = 0; i < 4; ++i) {
        const uint16_t h = FloatToHalf(unpacked[i], Round::TowardZero);
        if (FloatToHalf(unpacked[i], Round::NearestEven) != h || FloatToHalf(unpacked[i], Round::NearestAway) != h) return false;
        out[i] = HalfToFloat(h);
    }
    return true;
}

} // namespace gpu::edram_transfer
