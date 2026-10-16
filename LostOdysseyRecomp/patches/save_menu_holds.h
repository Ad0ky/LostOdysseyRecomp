#pragma once

#include <cstdint>

struct PPCContext;

namespace save_menu_holds
{
// Runs the title tick (sub_8234B9B8, r3 = title object) without its fixed
// waits: 1.0 s between the two save scans after Continue, and the profile
// check after START (0.5 s before the request, 1.0 s in all), whose message
// is not shown.
void TitleTick(PPCContext& ctx, uint8_t* base);
}
