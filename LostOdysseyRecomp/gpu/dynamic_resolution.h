#pragma once
#include <algorithm>
#include <cstdint>

// Dynamic render resolution (LO_DYNAMIC_RESOLUTION=1, opt-in): the scene's render height follows the GPU's busy time
// per frame. Above the frame budget it steps down a rung (36 rows), and with room to spare it steps back up, never
// above the configured height nor below the minimum. The presentation pass scales the scene to the output as it does
// for any render height (bicubic by default). Each change resizes the scene targets (a new geometry epoch), so changes
// are spaced out: down after about a second over budget, up after about three seconds of headroom.
namespace gpu::dynamic_resolution
{
    inline constexpr uint32_t Rung = 36;      // a multiple of 9: 16:9 widths stay whole (540 -> 960, 504 -> 896, ...)
    inline constexpr uint32_t Lowest = 360;
    // Heights resolution::ResolveInternalSize accepts besides its fixed modes.
    inline constexpr bool IsDynamicHeight(uint32_t height)
    {
        return height >= Lowest && height < 1080 && height % Rung == 0;
    }
    // The base heights it works from (the Switch's 540 and 720, and 1080).
    inline constexpr bool Supports(uint32_t base) { return base >= 540 && base <= 1080 && base % Rung == 0; }
    // The default minimum: 80% of the base height, on a rung (540 -> 432, 720 -> 576).
    inline constexpr uint32_t DefaultMinimum(uint32_t base) { return std::max(Lowest, (base * 4 / 5) / Rung * Rung); }

    struct Controller
    {
        // Tuning: the share of the frame budget the GPU may use before stepping down, and the share the next rung up
        // must stay under (its cost scaled by its pixel count) before stepping up.
        static constexpr double DownAt = 0.92, UpBelow = 0.80;
        static constexpr uint64_t WindowNs = 500'000'000ull, DownHoldNs = 1'000'000'000ull, UpHoldNs = 3'000'000'000ull;

        uint32_t base = 0, minimum = 0, height = 0;
        uint64_t windowStart = 0, windowBusy = 0, lastChange = 0;
        uint32_t windowFrames = 0, downStrikes = 0, upStrikes = 0;
        double lastBusyMs = 0;

        // Once per CPU frame. busyTotalNs: gpu_busy's running total. Returns the height to render at, and whether it
        // changed this frame (for the log).
        uint32_t Update(uint64_t nowNs, uint64_t busyTotalNs, uint32_t baseHeight, uint32_t minimumHeight, double budgetMs,
                        bool& changed)
        {
            changed = false;
            if (baseHeight != base || minimumHeight != minimum) {
                // Docked/undocked or reconfigured: start over at the full height.
                base = baseHeight;
                minimum = std::clamp(minimumHeight / Rung * Rung, Lowest, base);
                changed = height && height != base;
                height = base;
                Restart(nowNs, busyTotalNs);
                lastChange = nowNs;
                return height;
            }
            ++windowFrames;
            if (nowNs - windowStart < WindowNs)
                return height;
            const uint64_t busy = busyTotalNs - windowBusy;
            const uint32_t frames = windowFrames;
            Restart(nowNs, busyTotalNs);
            if (!busy || !frames)
                return height; // no timestamps (yet): keep the height
            const double busyMs = double(busy) / 1e6 / frames;
            lastBusyMs = busyMs;
            if (busyMs > budgetMs * DownAt) {
                upStrikes = 0;
                if (++downStrikes >= 2 && height > minimum && nowNs - lastChange >= DownHoldNs) {
                    // Two rungs when far over budget.
                    const uint32_t steps = busyMs > budgetMs * 1.25 ? 2u : 1u;
                    height = std::max(minimum, height - steps * Rung);
                    Changed(nowNs, changed);
                }
                return height;
            }
            downStrikes = 0;
            if (height < base) {
                const double up = double(height + Rung) / height;
                if (busyMs * up * up < budgetMs * UpBelow) {
                    if (++upStrikes >= 6 && nowNs - lastChange >= UpHoldNs) {
                        height += Rung;
                        Changed(nowNs, changed);
                    }
                } else {
                    upStrikes = 0;
                }
            }
            return height;
        }

    private:
        void Restart(uint64_t nowNs, uint64_t busyTotalNs)
        {
            windowStart = nowNs;
            windowBusy = busyTotalNs;
            windowFrames = 0;
        }
        void Changed(uint64_t nowNs, bool& changed)
        {
            changed = true;
            lastChange = nowNs;
            downStrikes = upStrikes = 0;
        }
    };
}
