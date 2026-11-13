#pragma once

#include "gpu_trace.h"
#include <plume_deko3d_api.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// LO_GPU_PASS_TRACE (gpu_trace.h): the log lines of a traced command list. For the files that record lists: the
// renderer and the presentation.
namespace gpu::gpu_trace
{
    inline const char* KindName(uint8_t kind)
    {
        using S = plume::Deko3DPassSegment;
        switch (kind) {
        case S::Draws: return "draws";
        case S::Clear: return "clear";
        case S::CopyImage: return "copy";
        case S::Upload: return "upload";
        case S::Readback: return "readback";
        case S::CopyBuffer: return "bufcopy";
        case S::Resolve: return "resolve";
        case S::Blit: return "blit2d";
        case S::Compute: return "compute";
        default: return "?";
        }
    }

    inline std::string FormatName(plume::RenderFormat format)
    {
        using F = plume::RenderFormat;
        switch (format) {
        case F::UNKNOWN: return "-";
        case F::R8G8B8A8_UNORM: return "rgba8";
        case F::B8G8R8A8_UNORM: return "bgra8";
        case F::R16G16B16A16_FLOAT: return "rgba16f";
        case F::R16G16B16A16_UNORM: return "rgba16";
        case F::R16G16B16A16_SNORM: return "rgba16s";
        case F::R10G10B10A2_UNORM: return "rgb10a2";
        case F::R11G11B10_FLOAT: return "r11g11b10f";
        case F::R16G16_FLOAT: return "rg16f";
        case F::R32_FLOAT: return "r32f";
        case F::R8_UNORM: return "r8";
        case F::D32_FLOAT: return "d32f";
        case F::D32_FLOAT_S8_UINT: return "d32fs8";
        default: return "f" + std::to_string(uint32_t(format));
        }
    }

    // What a texture is to its owner ("rt:...", "rs:...", "swap", "-" for none, "?" unknown). No spaces.
    using Namer = std::function<std::string(const plume::RenderTexture*)>;

    // Logs a list's stretches. listKind: S a renderer batch, P the presentation list. batchNs: the list's begin-to-end
    // time on the GPU.
    inline void LogPasses(const std::vector<plume::Deko3DPassSegment>& segments, char listKind, uint64_t frame, uint64_t batch,
        uint64_t batchNs, bool truncated, const Namer& name)
    {
        uint64_t traceNs = 0;
        uint32_t index = 0;
        std::string sampled;
        for (const auto& s : segments) {
            traceNs += s.gpuNs;
            sampled.clear();
            for (uint32_t i = 0; i < s.sampledCount; ++i) sampled += (i ? "," : "") + name(s.sampled[i]);
            if (sampled.empty()) sampled = "-";
            LOG_NOTICE("gpass: f={} l={} b={} i={} k={} us={:.1f} t={}x{} c={} d={} dr={} cl={} bar={}/{}/{} inv={} pb={} sb={} dw={} "
                "sp={} fr={} vt={} n={} rc={} rd={} src={}x{}:{}:{} by={} in={} el={} vs={:016x} ps={:016x}",
                frame, listKind, batch, index++, KindName(s.kind), double(s.gpuNs) / 1000.0, s.width, s.height,
                s.colorCount || s.kind != plume::Deko3DPassSegment::Draws ? FormatName(s.colorFormat) : "-",
                s.hasDepth ? FormatName(s.depthFormat) : "-", s.draws, s.clears, s.barriersFull, s.barriersFragments,
                s.barriersPrimitives, s.barriersImageInvalidate, s.pipelineBinds, s.stateBinds, s.descriptorWrites,
                s.samples, s.fragments, s.vertices, s.note[0] ? s.note : "-", name(s.color), name(s.depth),
                s.sourceWidth, s.sourceHeight, FormatName(s.sourceFormat), name(s.source), s.bytes, sampled, s.elements,
                s.vertexShader, s.pixelShader);
        }
        LOG_NOTICE("gpass end: f={} l={} b={} segs={} trace_us={:.1f} batch_us={:.1f} truncated={} counters={}",
            frame, listKind, batch, segments.size(), double(traceNs) / 1000.0, double(batchNs) / 1000.0, truncated ? 1 : 0,
            PassCounters() || PassDraws() ? 1 : 0);
    }
}
