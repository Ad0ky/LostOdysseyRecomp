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

    // LostOdysseyRecomp (2026-10-27): Deko3DOptions' GPU switches (plume_deko3d.h has what each does), before
    // CreateDeko3DInterface. All off by default, as banjo-nx ships them.
    struct Deko3DGpuOptions {
        bool depthCompression = false;
        bool zcull = false;
        bool mergeBarriers = false;
        bool leanBarriers = false;
        // Vertex shaders store only the outputs the pipeline's pixel shader reads (the translator's constant 905).
        bool vsOutputs = false;
        // Colour render targets with hardware compression (on by default).
        bool colorCompression = true;
    };
    void Deko3DSetGpuOptions(const Deko3DGpuOptions &options);
    // LostOdysseyRecomp (2026-10-29): the two switches read per barrier, changed while the game runs (the Debug page).
    // Call it on the thread that records the command lists, between them.
    void Deko3DSetBarrierOptions(bool mergeBarriers, bool leanBarriers);

    // LostOdysseyRecomp (2026-10-31): whether the list's next draws may test depth before their fragment shader. Only
    // for a shader that will not discard or write depth on those draws: the image is then the same, and hidden pixels
    // aren't shaded. Stays until changed.
    void Deko3DSetEarlyFragmentTests(RenderCommandList *list, bool allowed);

    // LostOdysseyRecomp (2026-11-13): optional draws (LO_RESOLVE_DROP_UNREAD). A draw, or a colour clear of one
    // rectangle of the bound target, recorded with these can still be cancelled (or enabled) until the list ends: the
    // 3D engine's render enable (SET_RENDER_ENABLE_C, FALSE or TRUE) is set around the draw or clear command alone, and
    // Deko3DSetOptional rewrites that one command word in command memory. Everything else the call records (pipeline
    // and buffer bindings, held barriers, the clear's scissors) is recorded before it and runs either way, so the list's
    // state is the same whether it is cancelled or not. A handle is good until the list's end() (0: none).
    // Deko3DOptionalAvailable: the start-up test of the render enable passed and the list records on this thread (not
    // the record thread). When not available, the calls record an ordinary draw or clear if `enabled` and return 0.
    bool Deko3DOptionalAvailable(RenderCommandList *list);
    uint64_t Deko3DDrawOptional(RenderCommandList *list, uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                uint32_t startInstance, bool enabled);
    uint64_t Deko3DClearColorOptional(RenderCommandList *list, uint32_t attachmentIndex, RenderColor color, const RenderRect &rect,
                                      bool enabled);
    bool Deko3DSetOptional(RenderCommandList *list, uint64_t handle, bool enabled);
    // The start-up test's result: -1 not run, 0 failed, 1 passed; got: the pixel read back after a clear under FALSE, a
    // TRUE rewritten to FALSE, and TRUE (RGBA8 words; expected A, A, B as the back end's log line says).
    int Deko3DRenderEnableTest(uint32_t got[3]);

    // LostOdysseyRecomp (2026-11-04): the CPU's copy of the uniform buffer at GPU 'address' (the draw's shared
    // constants), where bound textures read their sampler words; nullptr when the draw is done (plume_deko3d.h).
    bool Deko3DSetSharedWords(RenderCommandList *commandList, const void *cpu, uint32_t size, uint64_t address);

    // sa2-nx (2026-10-20): the swap chain's size, instead of the window's; 0, 0 goes back to the window's.
    void Deko3DSetSwapChainSize(uint32_t width, uint32_t height);

    // The back end's running totals (Deko3DStats), for the renderer's per-frame report.
    struct Deko3DCounters {
        uint64_t draws = 0, pipelineBinds = 0, stateBinds = 0, descriptorWrites = 0;
        uint64_t barriersPrimitives = 0, barriersFragments = 0, barriersFull = 0, barriersImageInvalidate = 0;
        uint64_t copyBuffer = 0, copyUpload = 0, copyReadback = 0, copyImage = 0, copyResolve = 0, copyBlit = 0;
        // Shader work on the console, on any thread: uam compiles and SPIR-V translations, and their time.
        uint64_t shaderCompiles = 0, shaderCompileNs = 0, translations = 0, translateNs = 0;
    };
    Deko3DCounters Deko3DReadCounters();

    // LostOdysseyRecomp (2026-11-11, issue #3): what deko3d holds of the heap, counted when LO_DEKO3D_HEAP_STATS=1
    // (on false otherwise): blocks made and freed, bytes held, and the 1 KiB control memory chunks made and freed.
    // selfCheck*: the chunks the start-up check's first list made, and those its next 99 made (0 with clear fixed).
    struct Deko3DHeapCounters {
        bool on = false;
        uint64_t allocs = 0, frees = 0, bytes = 0, chunkAllocs = 0, chunkFrees = 0;
        uint64_t selfCheckFirst = 0, selfCheckRest = 0;
    };
    Deko3DHeapCounters Deko3DReadHeapCounters();

    // LO_DEKO3D_BARRIER_TRACE: while on, every barrier that reaches the GPU is logged with what it waits for, its
    // textures, the draws recorded so far in the list and the caller (LostOdysseyRecomp, 2026-10-30).
    void Deko3DBarrierTrace(bool on);
}
