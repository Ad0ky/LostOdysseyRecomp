#include <debug/fast_forward.h>
#include <cstdio>
#include <cstdlib>

using debug_menu::fast_forward::Clock;
using debug_menu::fast_forward::GuestClockStartNs;

static void Check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main()
{
    // libnx's monotonic clock can have a Unix-sized value. Guest time must
    // start low enough that a 1/30 s tick remains visible as a float.
    constexpr uint64_t hostStart = 1800000000ull * 1000000000ull;
    constexpr uint64_t tick = 1000000000ull / 30;
    Clock clock;
    Check(clock.Read(hostStart) == GuestClockStartNs, "initial guest origin");
    Check(clock.origin == hostStart, "host origin retained for timestamp correction");
    Check(clock.Read(hostStart + tick) == GuestClockStartNs + tick, "normal tick advances");
    Check(float((clock.value + tick) / 1e9) > float(clock.value / 1e9), "float tick remains visible");

    clock.paused = true;
    Check(clock.Read(hostStart + 2 * tick) == GuestClockStartNs + tick, "pause freezes guest clock");
    clock.paused = false;
    clock.rate = 3;
    clock.lease = hostStart + 4 * tick;
    Check(clock.Read(hostStart + 3 * tick) == GuestClockStartNs + 4 * tick, "boost advances only after pause");
    Check(clock.Read(hostStart + 5 * tick) == GuestClockStartNs + 8 * tick, "boost ends at lease");
    Check(clock.rate == 1, "boost rate resets");
    std::puts("guest clock origin: passed");
}
