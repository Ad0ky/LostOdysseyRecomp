#pragma once

namespace render_fence
{
    // True while a guest thread has waited at least 250 ms on a render command
    // fence (FRenderCommandFence::Wait, 0x82322478): a loading screen whose
    // rendering thread is draining queued commands (cpu/poll_wait.cpp).
    bool LongWait();
}
