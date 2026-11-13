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

    // LostOdysseyRecomp (2026-11-13, LO_GPU_PASS_TRACE / LO_GPU_TIMELINE; gpu/gpu_trace.h, docs/SWITCH-GPU.md). While
    // either is set in the environment when a command list is made, the list can be traced: it writes a timestamp
    // report at the bottom of the pipe where the trace begins and ends and, in the modes that record them, at the end
    // of each stretch of its work (with counters: also the samples passed and the fragment and vertex shader
    // invocations so far). A stretch is the draws into one framebuffer, one clear call, one copy / resolve / blit /
    // upload, or one dispatch; a new note also ends one. The time of a stretch is the time between its report and
    // the one before: when its work left the pipe, not its share of a pipe it had to itself (the next stretch's
    // vertices are already under way). A copy's stretch ends after the barrier recorded behind it, which waits for
    // the copy engine. The reports add no wait. Nothing is traced with the record thread (LO_DEKO3D_RECORD_THREAD),
    // and the calls below do nothing for a list made without the traces.
    struct Deko3DPassSegment {
        enum Kind : uint8_t { Draws, Clear, CopyImage, Upload, Readback, CopyBuffer, Resolve, Blit, Compute };
        uint8_t kind = Draws;
        uint8_t colorCount = 0;
        bool hasDepth = false;
        uint32_t width = 0, height = 0;        // the framebuffer; a copy's rectangle
        uint32_t sourceWidth = 0, sourceHeight = 0;
        RenderFormat colorFormat = RenderFormat::UNKNOWN, depthFormat = RenderFormat::UNKNOWN, sourceFormat = RenderFormat::UNKNOWN;
        // The textures (colour attachment 0 or a copy's destination, the depth attachment, a copy's source), for the
        // caller to name: only compare them, they may be gone.
        const RenderTexture *color = nullptr, *depth = nullptr, *source = nullptr;
        uint32_t draws = 0, clears = 0;
        uint64_t bytes = 0;                    // a buffer copy's
        // Its draws' vertices or indices (count x instances), and the vertex and pixel shader of its last draw (a hash
        // of the shader as the pipeline was given it: its SPIR-V words, FNV-1a 64).
        uint64_t elements = 0;
        uint64_t vertexShader = 0, pixelShader = 0;
        // Its last draw's depth, stencil and colour state: bit 0 depth test, 1 depth write, 2 stencil test, 4-7 the
        // depth comparison (DkCompareOp), 8 the draw ran with early fragment tests, 9 its pipeline has an early
        // variant at all, 10 blending on target 0, 12-15 the colour write mask of target 0, 16 stencil writes
        // possible (an operation other than keep), 20-23 the stencil reference test (front, DkCompareOp).
        uint32_t state = 0;
        // The render targets its draws sample (those their shaders have a binding for), the first six.
        const RenderTexture *sampled[6] = {};
        uint32_t sampledCount = 0;
        // The back end's work recorded in the stretch (Deko3DCounters' differences).
        uint32_t barriersFull = 0, barriersFragments = 0, barriersPrimitives = 0, barriersImageInvalidate = 0;
        uint32_t pipelineBinds = 0, stateBinds = 0, descriptorWrites = 0;
        char note[40] = {};
        // From the reports (Deko3DPassTraceRead): the GPU's clock at the end, the time since the report before, and
        // the counters' growth over the stretch (0 without counters).
        uint64_t endNs = 0, gpuNs = 0;
        uint64_t samples = 0, fragments = 0, vertices = 0;
    };
    // Whether command lists are made traceable (LO_GPU_PASS_TRACE or LO_GPU_TIMELINE set, and no record thread).
    bool Deko3DPassTraceAvailable();
    // Starts tracing an open list (after begin()). mode 0: the list's begin and end only; 1: every stretch; 2: with
    // the counters; 3: with the counters and every draw a stretch of its own. False: not traced.
    bool Deko3DPassTraceBegin(RenderCommandList *list, uint32_t mode);
    // Names the work recorded from here on (at most 39 characters; nullptr: no name). Ends the stretch under way.
    void Deko3DPassTraceNote(RenderCommandList *list, const char *note);
    // Ends the trace, before end().
    void Deko3DPassTraceEnd(RenderCommandList *list);
    // The list's last trace, once its submission's fence has signalled: the GPU's clock where it began and ended,
    // and its stretches. truncated: the list had more stretches than the trace holds. False: the list was not traced
    // (or was read already).
    bool Deko3DPassTraceRead(RenderCommandList *list, std::vector<Deko3DPassSegment> &segments, uint64_t &beginNs,
                             uint64_t &endNs, bool &truncated);
    // The GPU's clock now, in the nanoseconds of timestamp queries (0: unavailable). A kernel call.
    uint64_t Deko3DGpuClockNs(RenderDevice *device);
}
