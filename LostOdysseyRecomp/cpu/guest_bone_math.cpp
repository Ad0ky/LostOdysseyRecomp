// Native versions of the bone-matrix loops of UE3's skeletal animation, the hottest game code in the first battle.
// USkeletalMeshComponent::ComposeSkeleton (sub_822B98C8, plus sub_822BA730) and the skinning-matrix loops of
// sub_822C3E40 multiply one FMatrix per bone. The recompiled loops copy both matrices to the stack through memory,
// build the columns and run 16 dot products; here it's one load, the multiply and one store per matrix.
// The mid-asm hooks return true when they did the work; LO_NATIVE_BONES=0 keeps the recompiled loops, for A/B runs.
#include <stdafx.h>
#include <kernel/memory.h>
#include <cstdlib>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>

namespace
{
    constexpr size_t kMatrixSize = 64;
    constexpr uint32_t kGuestIdentityMatrix = 0x8323A520; // FMatrix::Identity

    bool NativeBonesEnabled()
    {
        static const bool enabled = []()
            {
                const char* value = std::getenv("LO_NATIVE_BONES");
                return !(value && value[0] == '0');
            }();
        return enabled;
    }

    uint32_t LoadU32(uint8_t* base, uint32_t address)
    {
        return ByteSwap(*reinterpret_cast<uint32_t*>(base + address));
    }

    float32x4_t LoadRow(const uint8_t* p)
    {
        return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p)));
    }

    void StoreRow(uint8_t* p, float32x4_t row)
    {
        vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(row)));
    }

    void LoadMatrix(float32x4_t m[4], const uint8_t* p)
    {
        for (size_t i = 0; i < 4; ++i)
            m[i] = LoadRow(p + i * 16);
    }

    // out = a * b, row vectors, as FMatrix::operator*. Rounding can differ from the guest's dot products by an ulp.
    void MultiplyAndStore(uint8_t* out, const float32x4_t a[4], const float32x4_t b[4])
    {
        for (size_t i = 0; i < 4; ++i)
        {
            float32x4_t row = vmulq_laneq_f32(b[0], a[i], 0);
            row = vfmaq_laneq_f32(row, b[1], a[i], 1);
            row = vfmaq_laneq_f32(row, b[2], a[i], 2);
            row = vfmaq_laneq_f32(row, b[3], a[i], 3);
            StoreRow(out + i * 16, row);
        }
    }

    // sub_822BA730: quaternion (+0), translation (+16), scale (+28) to a matrix.
    void AtomToMatrix(float32x4_t m[4], const uint8_t* atom)
    {
        const float32x4_t rotation = LoadRow(atom);
        const float32x4_t translation = LoadRow(atom + 16);
        const float x = vgetq_lane_f32(rotation, 0);
        const float y = vgetq_lane_f32(rotation, 1);
        const float z = vgetq_lane_f32(rotation, 2);
        const float w = vgetq_lane_f32(rotation, 3);
        const float scale = vgetq_lane_f32(translation, 3);

        const float x2 = x * 2.0f, y2 = y * 2.0f, z2 = z * 2.0f;
        const float xx = x * x2, yy = y * y2, zz = z * z2;
        const float yz = y * z2, wx = w * x2;
        const float xy = x * y2, wz = w * z2;
        const float xz = x * z2, wy = w * y2;

        const float row0[4] = { 1.0f - (zz + yy), wz + xy, xz - wy, 0.0f };
        const float row1[4] = { xy - wz, 1.0f - (zz + xx), wx + yz, 0.0f };
        const float row2[4] = { wy + xz, yz - wx, 1.0f - (yy + xx), 0.0f };

        const uint32x4_t keepWZero = { ~0u, ~0u, ~0u, 0u };
        m[0] = vmulq_n_f32(vld1q_f32(row0), scale);
        m[1] = vmulq_n_f32(vld1q_f32(row1), scale);
        m[2] = vmulq_n_f32(vld1q_f32(row2), scale);
        m[3] = vsetq_lane_f32(1.0f, vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(translation), keepWZero)), 3);
    }
}

// sub_822B98C8 at 0x822B9A14, a bone with a parent: Matrix(atom[bone]) * world[parent] into world[bone].
bool NativeComposeBone(PPCRegister& r23, PPCRegister& r24, PPCRegister& r27)
{
    if (!NativeBonesEnabled())
        return false;

    uint8_t* base = g_memory.base;
    const uint32_t bone = r23.u32;
    const uint32_t world = LoadU32(base, r27.u32 + 708);
    const uint32_t bones = LoadU32(base, LoadU32(base, r27.u32 + 640) + 124);
    const uint32_t parent = LoadU32(base, bones + bone * 68 + 56);

    float32x4_t local[4], parentWorld[4];
    AtomToMatrix(local, base + LoadU32(base, r24.u32) + bone * 32);
    LoadMatrix(parentWorld, base + world + parent * kMatrixSize);
    MultiplyAndStore(base + world + bone * kMatrixSize, local, parentWorld);
    return true;
}

namespace
{
    // refToLocal[bone] * bases[index] into out[bone] for every bone of the LOD, identity when the index is invalid.
    // The bone map is only there for a mesh that follows a master component's pose (r31 at the first hook).
    void SkinningMatrices(PPCRegister& r27, PPCRegister& r29, PPCRegister& r30, uint32_t basesOwner, uint32_t boneMap)
    {
        uint8_t* base = g_memory.base;
        const uint32_t lod = r27.u32;
        const uint32_t skeleton = r29.u32;

        for (int32_t i = 0; i < int32_t(LoadU32(base, lod + 28)); ++i)
        {
            const uint32_t list = LoadU32(base, lod + 24);
            const uint32_t bone = ByteSwap(*reinterpret_cast<uint16_t*>(base + list + i * 2));
            const int32_t index = boneMap ? int32_t(LoadU32(base, boneMap + bone * 4)) : int32_t(bone);
            uint8_t* out = base + LoadU32(base, r30.u32) + bone * kMatrixSize;

            if (int32_t(bone) < int32_t(LoadU32(base, skeleton + 176)) && index >= 0 && index < int32_t(LoadU32(base, basesOwner + 712)))
            {
                float32x4_t refToLocal[4], bases[4];
                LoadMatrix(refToLocal, base + LoadU32(base, skeleton + 172) + bone * kMatrixSize);
                LoadMatrix(bases, base + LoadU32(base, basesOwner + 708) + uint32_t(index) * kMatrixSize);
                MultiplyAndStore(out, refToLocal, bases);
            }
            else
            {
                std::memcpy(out, base + kGuestIdentityMatrix, kMatrixSize);
            }
        }
    }
}

// sub_822C3E40 at 0x822C3ED8: the pose comes from the master component in r31, through the bone map at r28+748.
bool NativeSkinningMatricesMaster(PPCRegister& r27, PPCRegister& r28, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31)
{
    if (!NativeBonesEnabled())
        return false;

    SkinningMatrices(r27, r29, r30, r31.u32, LoadU32(g_memory.base, r28.u32 + 748));
    return true;
}

// sub_822C3E40 at 0x822C40F0: the component's own pose.
bool NativeSkinningMatricesOwn(PPCRegister& r27, PPCRegister& r28, PPCRegister& r29, PPCRegister& r30)
{
    if (!NativeBonesEnabled())
        return false;

    SkinningMatrices(r27, r29, r30, r28.u32, 0);
    return true;
}

#else

bool NativeComposeBone(PPCRegister&, PPCRegister&, PPCRegister&) { return false; }
bool NativeSkinningMatricesMaster(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) { return false; }
bool NativeSkinningMatricesOwn(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) { return false; }

#endif
