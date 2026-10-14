// Switch: run the game's two XAudio worker threads above the game threads.
// The audio driver thread (priority 0x2C) calls the guest audio callback,
// which signals these workers and waits for them to mix the voices. Guest
// thread priorities are not mapped on the Switch (KeSetBasePriorityThread is
// a no-op there), so the workers run at libnx's default 0x3B and share the
// cores with every game thread; when cutscene video decoding keeps the cores
// busy the callback waits too long and the audio driver misses ticks (heard
// as stutter). LO_XAUDIO_WORKER_PRIORITY=<hex> overrides, 0 keeps 0x3B.
#include <stdafx.h>

#if LO_PLATFORM_SWITCH

#include <os/logger.h>
#include <os/switch_platform.h>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_82CC4D98);
extern "C" PPC_FUNC(__imp__sub_82CC4E60);

namespace
{
int WorkerPriority()
{
    static const int priority = [] {
        const char* value = std::getenv("LO_XAUDIO_WORKER_PRIORITY");
        return value && *value ? int(std::strtol(value, nullptr, 16)) : 0x2D;
    }();
    return priority;
}

void Raise(const char* which)
{
    const int priority = WorkerPriority();
    if (priority < 0x2C || priority > 0x3F) return;
    os::switch_platform::SetCurrentThreadPriority(priority);
    LOG_INFO("xaudio: {} worker thread priority {:#x}", which, priority);
}
}

// Thread entry functions (guest threads "82cc4d98" and "82cc4e60"); they loop
// until the audio engine shuts down.
PPC_FUNC(sub_82CC4D98)
{
    Raise("first");
    __imp__sub_82CC4D98(ctx, base);
}

PPC_FUNC(sub_82CC4E60)
{
    Raise("second");
    __imp__sub_82CC4E60(ctx, base);
}

#endif
