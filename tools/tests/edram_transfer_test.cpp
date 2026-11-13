// The per-pair EDRAM transfer shaders (gpu/edram_transfer.h kTransferPairHlsl) against the general one
// (kTransferHlsl), through their CPU twins, for every input that can reach them:
//   - Float32To7e3 old (branches, as the console's compiler runs it: NaN gives 0x3FF, see edram_transfer.h)
//     == new (selects) for every one of the 65,536 FP16 bit patterns as the shader reads them from the
//     RGBA16F source (negatives, infinities and NaNs included), and for every one of the 2^32 FP32 bit
//     patterns besides; the source's own NaN rule (NaN gives 0) is counted apart and differs only for NaN;
//   - Float7e3To32 old == new, bit for bit, for all 1,024 codes;
//   - the unorm and alpha quantisers: saturate(x) * scale + 0.5 gives the same integer with and without a
//     fused multiply-add for every FP16 input (the product is exact), so a GPU compiler's contraction
//     choice cannot tell the two shaders apart;
//   - each of the nine (source, destination) class pairs, pack and unpack together, on 65,536 texels whose
//     four channels each take every FP16 value once.
// Prints the counts and exits 1 on any difference.
#include "gpu/edram_transfer.h"
#include <bit>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace et = gpu::edram_transfer;

namespace {

// The pair shader's main(), on the CPU: kTransferPairHlsl with the class tests resolved.
uint32_t PackPair(const float v[4], uint32_t cls)
{
    if (cls == 0) {
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i) w |= et::Quantize(v[i], 255.0f, false) << (8 * i);
        return w;
    }
    const uint32_t a = et::Quantize(v[3], 3.0f, false);
    uint32_t c[3];
    for (int i = 0; i < 3; ++i) c[i] = cls == 1 ? et::Quantize(v[i], 1023.0f, false) : et::Float32To7e3Select(v[i]);
    return c[0] | (c[1] << 10) | (c[2] << 20) | (a << 30);
}

void UnpackPair(uint32_t w, uint32_t cls, float out[4])
{
    if (cls == 0) {
        for (int i = 0; i < 4; ++i) out[i] = float((w >> (8 * i)) & 0xFFu) * float(1.0 / 255.0);
        return;
    }
    out[3] = float(w >> 30) * float(1.0 / 3.0);
    for (int i = 0; i < 3; ++i) {
        const uint32_t d = (w >> (10 * i)) & 0x3FFu;
        out[i] = cls == 1 ? float(d) * float(1.0 / 1023.0) : et::Float7e3To32Select(d);
    }
}

uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }

} // namespace

int main()
{
    uint64_t failures = 0;

    // Float32To7e3 over every FP16 input, then over every FP32 bit pattern.
    {
        uint32_t diff = 0, nan = 0, inf = 0, negative = 0, saturated = 0, denormalPath = 0, sourceRule = 0, sourceNan = 0;
        for (uint32_t h = 0; h < 0x10000u; ++h) {
            const float f = et::HalfToFloat(uint16_t(h));
            const uint32_t a = et::Float32To7e3Compiled(f), b = et::Float32To7e3Select(f);
            if (a != b) {
                if (diff < 8) std::printf("  Float32To7e3 half %04x: %03x vs %03x\n", h, a, b);
                ++diff;
            }
            if (et::Float32To7e3(f) != b) {
                ++sourceRule;
                sourceNan += f != f;
            }
            nan += f != f;
            inf += (h & 0x7FFFu) == 0x7C00u;
            negative += (h & 0x8000u) != 0;
            saturated += a == 0x3FFu;
            denormalPath += f > 0.0f && Bits(f) < 0x3E800000u;
        }
        std::printf("Float32To7e3 as compiled, 65536 FP16 inputs (%u NaN, %u infinities, %u with the sign bit, %u saturate "
                    "to 0x3FF, %u through the 7e3 denormal path): %u differ\n", nan, inf, negative, saturated, denormalPath, diff);
        std::printf("  the source's NaN rule instead: %u differ, %u of them NaN\n", sourceRule, sourceNan);
        failures += diff + (sourceRule - sourceNan);
        uint64_t diff32 = 0, sourceRule32 = 0, sourceNan32 = 0;
        uint32_t u = 0;
        do {
            const float f = std::bit_cast<float>(u);
            const uint32_t b = et::Float32To7e3Select(f);
            if (et::Float32To7e3Compiled(f) != b) {
                if (diff32 < 8) std::printf("  Float32To7e3 fp32 %08x differs\n", u);
                ++diff32;
            }
            if (et::Float32To7e3(f) != b) {
                ++sourceRule32;
                sourceNan32 += f != f;
            }
        } while (++u != 0);
        std::printf("Float32To7e3 as compiled, all 4294967296 FP32 bit patterns: %" PRIu64 " differ\n", diff32);
        std::printf("  the source's NaN rule instead: %" PRIu64 " differ, %" PRIu64 " of them NaN\n", sourceRule32, sourceNan32);
        failures += diff32 + (sourceRule32 - sourceNan32);
    }

    // Float7e3To32 over all 1,024 codes (and the masking of the bits above them).
    {
        uint32_t diff = 0;
        for (uint32_t f10 = 0; f10 < 0x400u * 64u; ++f10) {
            const uint32_t input = (f10 & 0x3FFu) | ((f10 >> 10) << 26);
            const uint32_t a = Bits(et::Float7e3To32(input)), b = Bits(et::Float7e3To32Select(input));
            if (a != b) {
                if (diff < 8) std::printf("  Float7e3To32 %08x: %08x vs %08x\n", input, a, b);
                ++diff;
            }
        }
        std::printf("Float7e3To32, 1024 codes x 64 patterns of the bits above them: %u differ\n", diff);
        failures += diff;
    }

    // The quantisers: fused and separate multiply-add agree for every FP16 input.
    for (float scale : {255.0f, 1023.0f, 3.0f}) {
        uint32_t diff = 0;
        for (uint32_t h = 0; h < 0x10000u; ++h) {
            const float f = et::HalfToFloat(uint16_t(h));
            if (f != f) continue; // saturate(NaN) is the GPU's to define; both shaders run the same instruction on it
            if (et::Quantize(f, scale, false) != et::Quantize(f, scale, true)) ++diff;
        }
        std::printf("Quantize x %.0f, 63488 non-NaN FP16 inputs, separate vs fused multiply-add: %u differ\n", scale, diff);
        failures += diff;
    }

    // Every pair of classes: general pack + unpack against the pair shader's, each channel through every FP16 value.
    for (uint32_t s = 0; s <= et::kLastClass; ++s)
        for (uint32_t d = 0; d <= et::kLastClass; ++d) {
            uint32_t wordDiff = 0, outDiff = 0;
            for (uint32_t i = 0; i < 0x10000u; ++i) {
                // Four bijections of i: each channel sees each FP16 pattern once.
                const uint16_t hs[4] = {uint16_t(i), uint16_t(i * 40503u + 7u), uint16_t(~i), uint16_t((i << 8) | (i >> 8))};
                float v[4];
                for (int c = 0; c < 4; ++c) {
                    v[c] = et::HalfToFloat(hs[c]);
                    if (v[c] != v[c]) v[c] = 0.0f; // saturate(NaN): see above; NaN through 7e3 is covered by the first check
                }
                const uint32_t wa = et::PackGuest(v, s, false), wb = PackPair(v, s);
                wordDiff += wa != wb;
                float a[4], b[4];
                et::UnpackGuest(wa, d, a);
                UnpackPair(wb, d, b);
                if (std::memcmp(a, b, sizeof(a)) != 0) ++outDiff;
            }
            std::printf("pair %u -> %u, 65536 texels: %u words differ, %u outputs differ\n", s, d, wordDiff, outDiff);
            failures += wordDiff + outDiff;
        }

    std::printf("%s\n", failures ? "FAILED" : "all equal");
    return failures ? 1 : 0;
}
