#pragma once

#include <os/logger.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

// GPU analysis traces for the Switch's deko3d back end (docs/SWITCH-GPU.md). Both are off unless env.txt turns them on,
// and neither adds a GPU wait: results are read after the fences the renderer waits for anyway.
//
//   LO_GPU_PASS_TRACE=N     every Nth frame, the renderer's command lists and the presentation list close each stretch of
//                           their work (the draws into one set of render targets, a clear, a copy, a resolve) with a
//                           counter report, and the log gets one "gpass:" line per stretch with its GPU time.
//                           LO_GPU_PASS_COUNTERS=1 adds the samples passed and the fragment and vertex shader
//                           invocations of each stretch, LO_GPU_PASS_DRAWS=1 makes every draw a stretch (with the
//                           counters and its shaders). tools/switch/gpu_passes.py adds the lines up.
//   LO_GPU_TIMELINE=1       one "gtl:" line per presented frame: when the render thread submitted each command list,
//                           when the GPU began and finished it (its own clock), every wait of the render thread (fences,
//                           the swapchain image, the present, pacing, the game), and pairs of the two clocks.
//                           tools/switch/gpu_timeline.py turns them into the GPU's busy and idle time per frame and
//                           what the render thread was doing while the GPU had nothing to do.
//
// The hooks are in code that runs once per command list or per frame (video.cpp's begin, end, submit and fence wait,
// perf_stats.cpp's frame and idle counters) and in a command list class of their own (plume_deko3d.h,
// Deko3DTracedCommandList), never in the draw path: a function whose flow graph changes loses its PGO profile, and
// hooks in the command processor and the renderer's batch functions cost 4% FPS in town with both traces off.
namespace gpu::gpu_trace
{
    inline uint64_t EnvNumber(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? std::strtoull(value, nullptr, 10) : 0ull;
    }

    // Frames between traced frames (0: off). 1 traces every frame.
    inline uint64_t PassEvery()
    {
        static const uint64_t every = EnvNumber("LO_GPU_PASS_TRACE");
        return every;
    }
    inline bool PassCounters()
    {
        static const bool on = EnvNumber("LO_GPU_PASS_COUNTERS") != 0;
        return on;
    }
    // LO_GPU_PASS_DRAWS=1: every draw of a traced frame is a stretch of its own, with the counters: a line per draw
    // (about 1,300 a frame in the first battle), so trace few frames.
    inline bool PassDraws()
    {
        static const bool on = EnvNumber("LO_GPU_PASS_DRAWS") != 0;
        return on;
    }
    inline bool PassFrame(uint64_t frame) { return PassEvery() && frame && frame % PassEvery() == 0; }

    // Presented frames so far (perf_stats::Present counts them): the frame number of the traces.
    inline std::atomic<uint64_t> g_frame{0};
    inline uint64_t Frame() { return g_frame.load(std::memory_order_relaxed); }

    inline bool Timeline()
    {
        static const bool on = EnvNumber("LO_GPU_TIMELINE") != 0;
        return on;
    }

    // LO_GPU_EXPERIMENT=<bits>: diagnostic what-ifs for sizing a cost, never for playing (some change the image).
    //   1  the presentation's final pass without the game's display gamma ramp
    //   2  the presentation's final pass without the player's brightness / gamma curve
    //   16 traced lists (either trace on): no descriptor or shader cache invalidation before draws (plume_deko3d_commands.cpp)
    //   32 traced lists: that invalidation without the shader caches
    inline uint64_t Experiment()
    {
        static const uint64_t bits = EnvNumber("LO_GPU_EXPERIMENT");
        return bits;
    }

    // Microseconds on the CPU's clock since the first call.
    inline std::chrono::steady_clock::time_point Epoch()
    {
        static const auto epoch = std::chrono::steady_clock::now();
        return epoch;
    }
    inline uint64_t ToUs(std::chrono::steady_clock::time_point time)
    {
        const auto epoch = Epoch();
        return time > epoch ? uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(time - epoch).count()) : 0;
    }
    inline uint64_t NowUs() { return ToUs(std::chrono::steady_clock::now()); }

    // The timeline's events of the frame being recorded. The render thread writes nearly all of them; the lock is for
    // the few waits other threads make.
    struct TimelineState
    {
        std::mutex mutex;
        std::string line;
        uint64_t nextId = 1;
        uint64_t lastClockUs = 0;
    };
    inline TimelineState& State()
    {
        static TimelineState state;
        return state;
    }

    inline void Append(const char* text)
    {
        auto& s = State();
        const std::scoped_lock lock(s.mutex);
        if (s.line.size() < 3000) s.line += text;
    }

    // A wait of the calling thread: kind is one letter (F renderer fence, Q presentation fence, A swapchain image,
    // X present, Z pacing sleep, I waiting for the game's commands). A submit call's time is in its S / P field.
    inline void Wait(char kind, uint64_t startUs, uint64_t endUs)
    {
        if (endUs < startUs + 20) return; // shorter than 20 us: not worth a field
        char text[48];
        std::snprintf(text, sizeof(text), " %c%llu:%llu", kind, static_cast<unsigned long long>(startUs),
            static_cast<unsigned long long>(endUs - startUs));
        Append(text);
    }
    struct ScopedWait
    {
        char kind;
        bool on;
        uint64_t start;
        explicit ScopedWait(char kind) : kind(kind), on(Timeline()), start(on ? NowUs() : 0) {}
        ~ScopedWait() { if (on) Wait(kind, start, NowUs()); }
        ScopedWait(const ScopedWait&) = delete;
        ScopedWait& operator=(const ScopedWait&) = delete;
    };

    // A submission: kind S (renderer batch) or P (presentation list). Returns its id for GpuTimes().
    inline uint64_t Submitted(char kind, uint64_t startUs, uint64_t endUs, uint64_t frame)
    {
        auto& s = State();
        uint64_t id;
        {
            const std::scoped_lock lock(s.mutex);
            id = s.nextId++;
        }
        char text[80];
        std::snprintf(text, sizeof(text), " %c%llu:%llu:%llu:%llu", kind, static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(startUs), static_cast<unsigned long long>(endUs - startUs),
            static_cast<unsigned long long>(frame));
        Append(text);
        return id;
    }

    // The GPU's timestamps (ns, its clock) at the start and end of submission id.
    inline void GpuTimes(uint64_t id, uint64_t beginNs, uint64_t endNs)
    {
        if (!id) return;
        char text[80];
        std::snprintf(text, sizeof(text), " G%llu:%llu:%llu", static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(beginNs / 1000), static_cast<unsigned long long>(endNs / 1000));
        Append(text);
    }

    // A reading of the GPU's clock (ns) taken between two readings of the CPU's.
    inline void Clock(uint64_t beforeUs, uint64_t afterUs, uint64_t gpuNs)
    {
        if (!gpuNs) return;
        char text[80];
        std::snprintf(text, sizeof(text), " O%llu:%llu:%llu", static_cast<unsigned long long>(beforeUs),
            static_cast<unsigned long long>(afterUs - beforeUs), static_cast<unsigned long long>(gpuNs / 1000));
        Append(text);
    }
    // Whether a clock pair is due (twice a second).
    inline bool ClockDue()
    {
        if (!Timeline()) return false;
        auto& s = State();
        const uint64_t now = NowUs();
        const std::scoped_lock lock(s.mutex);
        if (s.lastClockUs && now - s.lastClockUs < 500000) return false;
        s.lastClockUs = now;
        return true;
    }

    // The end of a presented frame: counts it and writes the timeline's line.
    inline void Swap(uint64_t swap)
    {
        g_frame.store(g_frame.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (!Timeline()) return;
        auto& s = State();
        std::string line;
        {
            const std::scoped_lock lock(s.mutex);
            line.swap(s.line);
        }
        LOG_NOTICE("gtl: n={} t={}{}", swap, NowUs(), line);
    }
}
