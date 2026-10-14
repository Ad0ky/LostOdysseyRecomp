#include <stdafx.h>
#include "poll_wait.h"
#include "render_fence.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <os/logger.h>
#include <patches/occlusion_history.h>

extern "C" PPC_FUNC(__imp__sub_823CF390);
extern "C" PPC_FUNC(__imp__sub_82322478);

// Retain the generated function, including register saves, return values and
// query ordering. Only its repeated not-ready probes can pause on the host.
PPC_FUNC(sub_823CF390)
{
    // Policy::History answers the game's deferred reads without the loop.
    if (occlusion_history::Read(ctx, base)) return;
    poll_wait::RunScoped(poll_wait::Kind::Query, [&] { __imp__sub_823CF390(ctx, base); });
}

// Loading screens. While the game thread waits on a render command fence
// (FlushRenderingCommands during a map or battle load), the rendering thread
// (RenderingThreadMain 0x824856A0) runs one queued command, then ticks the
// loading-screen movie (0x823B63E0) whenever its tick interval has passed.
// Each tick presents a frame and waits for the GPU to be at most 3 frames
// behind; with presentation paced at 30 fps that wait alone outlasts the
// interval, so the queue drains at about one command per frame and a map load
// takes up to a minute. On the console the GPU presents at 60 Hz and the tick
// rarely waits. render_fence::LongWait() lets the command processor pace at
// 60 fps while a fence wait has lasted 250 ms (gpu/command_processor.cpp).
// Skipping ticks instead (LO_LOAD_TICK_THROTTLE=1, at most one tick every
// 100 ms between commands) left battle transitions on the loading screen for
// minutes on 2026-10-14, so it is off by default.
namespace
{
    std::atomic<int> g_fenceWaiters{0};
    std::atomic<uint64_t> g_fenceWaitSinceMs{0};
    std::atomic<uint64_t> g_lastMovieTickMs{0};
    std::atomic<uint32_t> g_ticksRun{0}, g_ticksDeferred{0};

    uint64_t NowMs()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    bool TickThrottleEnabled()
    {
        static const bool on = [] {
            const char* v = getenv("LO_LOAD_TICK_THROTTLE");
            return v && v[0] == '1';
        }();
        return on;
    }
}

bool render_fence::LongWait()
{
    const uint64_t since = g_fenceWaitSinceMs.load(std::memory_order_acquire);
    return since && NowMs() - since >= 250;
}

// FRenderCommandFence::Wait: this guest loop repeatedly invokes Sleep(0) while
// a shared value exceeds its threshold. Keep its original reads and comparison;
// opt in only this call tree. Guest thread exit uses longjmp: no objects with
// destructors here.
PPC_FUNC(sub_82322478)
{
    if (g_fenceWaiters.fetch_add(1, std::memory_order_acq_rel) == 0)
        g_fenceWaitSinceMs.store(NowMs(), std::memory_order_release);
    poll_wait::RunScoped(poll_wait::Kind::SharedValue, [&] { __imp__sub_82322478(ctx, base); });
    if (g_fenceWaiters.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        const uint64_t since = g_fenceWaitSinceMs.exchange(0, std::memory_order_acq_rel);
        const uint32_t run = g_ticksRun.exchange(0), deferred = g_ticksDeferred.exchange(0);
        if (since && NowMs() - since >= 1000)
            LOG_INFO("render fence wait {:.1f} s: loading-screen ticks run {}, deferred {}",
                (NowMs() - since) / 1000.0, run, deferred);
    }
}

extern "C" PPC_FUNC(__imp__sub_823B63E0);
PPC_FUNC(sub_823B63E0)
{
    // Only the call between two queued commands (not the idle-loop tick).
    if (uint32_t(ctx.lr) == 0x824857A4)
    {
        const uint64_t since = g_fenceWaitSinceMs.load(std::memory_order_acquire);
        const uint64_t now = NowMs();
        if (TickThrottleEnabled() && since && now - since >= 250 &&
            now - g_lastMovieTickMs.load(std::memory_order_relaxed) < 100)
        {
            g_ticksDeferred.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_ticksRun.fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_823B63E0(ctx, base);
    g_lastMovieTickMs.store(NowMs(), std::memory_order_relaxed);
}

// GpuPoll for the title GPU timestamp wait lives in debug/gpu_wait_trace.cpp.
// That file already owns PPC_FUNC(sub_823B62A0) and PPC_FUNC(sub_827B6278).
