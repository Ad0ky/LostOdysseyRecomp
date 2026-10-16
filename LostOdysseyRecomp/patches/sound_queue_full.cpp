#include <stdafx.h>
#include <atomic>
#include <chrono>
#include <os/logger.h>

// #497/#499: a stream must not stay stuck when the sound command queue is full.
//
// The sound system (*0x832D268C) hands stops and releases to its worker thread
// through a ring of 192 messages (+20324; +4 count, +6 capacity). The worker
// (sub_82B632C0) takes one message per wake. When the ring is full,
// sub_8236CEE8 returns 0 and the request is lost:
// - Release (sub_8236C7D8): the stream slot (+3580, 20 bytes each) gets busy
//   bits 0xC0 at +3 and state 4, then state 3 when message 11 cannot be
//   queued. The sound tick retries state 3 (sub_82B62610 / 0x82B62760), but
//   the release returns at once when +3 bit 0x80 is set, so the retry never
//   queues anything. The slot stays taken; once all six are taken, new music
//   and cutscene audio cannot start ("stream start failed (no free stream
//   slot)" with state=3 slots).
// - Stop (sub_82B62048): returns 0 when message 14 cannot be queued; the
//   stream keeps playing, a looping track until the next battle.
//
// Let the tick's retry work: clear the busy bits of a state-3 stream slot
// before the release, so it queues message 11 again until it fits. Turn a
// stop that could not be queued into a release, which then retries the same
// way. Such a stream ends without its fade.

extern "C" PPC_FUNC(__imp__sub_8236C7D8);
extern "C" PPC_FUNC(__imp__sub_82B62048);
extern "C" PPC_FUNC(__imp__sub_8236CEE8);
PPC_FUNC(sub_8236C7D8);

namespace
{
constexpr uint32_t SoundSystemPointer = 0x832D268C;
constexpr uint32_t StreamSlots = 3580;
constexpr uint32_t SlotSize = 20;
constexpr uint32_t StreamSlotCount = 6;
constexpr uint32_t StreamKind = 3;
constexpr uint8_t ReleaseFailed = 3;
constexpr uint8_t Releasing = 0x80;
constexpr uint8_t BusyBits = 0xC0;
constexpr uint8_t Playing = 6;
constexpr uint8_t Ended = 12;

// The slot of a live stream handle (index | kind << 8 | generation << 12;
// bits 20-27 select other paths in sub_8236C7D8), or 0.
uint32_t StreamSlot(uint8_t* base, uint32_t system, uint32_t handle)
{
    if (system != PPC_LOAD_U32(SoundSystemPointer) || ((handle >> 20) & 0xFF) != 0 || ((handle >> 8) & 0xF) != StreamKind)
        return 0;
    const uint32_t index = handle & 0xFF;
    if (index >= StreamSlotCount)
        return 0;
    const uint32_t slot = system + StreamSlots + index * SlotSize;
    if (!PPC_LOAD_U8(slot) || PPC_LOAD_U8(slot + 1) != ((handle >> 12) & 0xFF))
        return 0;
    return slot;
}

bool Due(std::atomic<int64_t>& last)
{
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t previous = last.load(std::memory_order_relaxed);
    return now - previous >= 10 && last.compare_exchange_strong(previous, now, std::memory_order_relaxed);
}
}

// sub_8236C7D8(sound system, handle): release.
PPC_FUNC(sub_8236C7D8)
{
    const uint32_t slot = StreamSlot(base, ctx.r3.u32, ctx.r4.u32);
    if (slot && PPC_LOAD_U8(slot) == ReleaseFailed && (PPC_LOAD_U8(slot + 3) & Releasing))
        PPC_STORE_U8(slot + 3, uint8_t(PPC_LOAD_U8(slot + 3) & ~BusyBits));
    __imp__sub_8236C7D8(ctx, base);
}

// sub_82B62048(sound system, handle, mode, ...; f1 fade): stop.
PPC_FUNC(sub_82B62048)
{
    const uint32_t system = ctx.r3.u32, handle = ctx.r4.u32;
    __imp__sub_82B62048(ctx, base);
    if (ctx.r3.u32)
        return;
    const uint32_t slot = StreamSlot(base, system, handle);
    // Returns 0 for a live, playing stream only when message 14 was not queued.
    if (!slot || PPC_LOAD_U8(slot) != Playing || PPC_LOAD_U8(slot + 9) == Ended || (PPC_LOAD_U8(slot + 3) & Releasing))
        return;
    static std::atomic<int64_t> lastLog{0};
    if (Due(lastLog))
        LOG_WARNING("sound queue full: stream stop {:#x} replaced by a release", handle);
    const PPCContext saved = ctx;
    ctx.r3.u64 = system;
    ctx.r4.u64 = handle;
    sub_8236C7D8(ctx, base);
    ctx = saved;
    ctx.r3.u64 = 1;
}

// sub_8236CEE8(ring, index out): queues a message, 0 when the ring is full.
PPC_FUNC(sub_8236CEE8)
{
    const uint32_t ring = ctx.r3.u32;
    __imp__sub_8236CEE8(ctx, base);
    if (ctx.r3.u32)
        return;
    static std::atomic<int64_t> lastLog{0};
    if (Due(lastLog))
        LOG_WARNING("sound queue full: ring {:#x} holds {} of {} messages", ring, PPC_LOAD_U16(ring + 4), PPC_LOAD_U16(ring + 6));
}
