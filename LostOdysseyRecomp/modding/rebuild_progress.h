#pragma once

#include <atomic>
#include <cstdint>

// The rebuild of a folder's files for the mods (modding/text_overlay.cpp), which
// the game waits for on the first start after the mods change: its worker
// threads count packages here and the window shows the count (gpu/video.cpp).
namespace modding::rebuild_progress
{
    // total << 32 | done; 0 while no rebuild runs.
    inline std::atomic<uint64_t> g_progress{0};

    // On a given counter: the background preparation of other discs counts on
    // its own, so the window does not show it while the game runs.
    inline void AddTotal(std::atomic<uint64_t> &progress, uint32_t packages) { progress.fetch_add(uint64_t(packages) << 32, std::memory_order_relaxed); }
    inline void Advance(std::atomic<uint64_t> &progress) { progress.fetch_add(1, std::memory_order_relaxed); }
    inline void Finish(std::atomic<uint64_t> &progress) { progress.store(0, std::memory_order_relaxed); }

    inline void AddTotal(uint32_t packages) { AddTotal(g_progress, packages); }
    inline void Advance() { Advance(g_progress); }
    inline void Finish() { Finish(g_progress); }

    inline uint32_t Done(uint64_t progress) { return uint32_t(progress); }
    inline uint32_t Total(uint64_t progress) { return uint32_t(progress >> 32); }
}
