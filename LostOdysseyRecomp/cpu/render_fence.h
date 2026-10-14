#pragma once
#include <cstdint>

namespace render_fence
{
    // True while a guest thread has waited at least 250 ms on a render command
    // fence (FRenderCommandFence::Wait, 0x82322478): a loading screen whose
    // rendering thread is draining queued commands (cpu/poll_wait.cpp).
    bool LongWait();
    // Rendering thread, around each queued command (gpu/frame_plan.cpp hooks
    // at 0x824857C8 / 0x824857F8): timed only during a fence wait.
    void CommandBegin(uint32_t command);
    void CommandEnd();
}
