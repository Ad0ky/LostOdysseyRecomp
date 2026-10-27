//
// LostOdysseyRecomp (2026-10-27): what the application calls of plume's deko3d back end, without <deko3d.h> (it
// brings libnx's <switch.h>, whose Mutex/Event/Thread typedefs collide with the runtime's). plume_deko3d.h includes
// this, so the declarations cannot drift.
//
#pragma once

#include "plume_render_interface.h"
#include "plume_deko3d_spirv.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace plume {
    // What a DKSH cache gives the device: the DKSH for a variant's full GLSL text (its hash), or null.
    using Deko3DShaderCacheLookup = std::function<bool(uint64_t sourceHash, std::vector<uint8_t> &dksh)>;
    // Called with every DKSH the device compiled (to keep in a cache).
    using Deko3DShaderCacheStore = std::function<void(uint64_t sourceHash, const std::vector<uint8_t> &dksh)>;
    void SetDeko3DShaderCache(Deko3DShaderCacheLookup lookup, Deko3DShaderCacheStore store);
    // Where the back end's messages go (stderr until set).
    void SetDeko3DLogCallback(void (*callback)(const char *message));
    // What the SPIR-V translation assumes (the address window), before shaders are made.
    void Deko3DSetSpirvOptions(const Deko3DSpirvOptions &options);
    std::unique_ptr<RenderInterface> CreateDeko3DInterface();
}
