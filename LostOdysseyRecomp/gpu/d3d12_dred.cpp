#include "d3d12_dred.h"
#ifdef _WIN32
#include <plume_d3d12.h>
#include <os/logger.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <string>

namespace gpu::dred
{
    namespace
    {
        std::atomic<bool> g_enabled{false};
        std::atomic<bool> g_logged{false};

        const char* OpName(D3D12_AUTO_BREADCRUMB_OP op)
        {
            static constexpr const char* names[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced",
                "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion",
                "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView",
                "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
                "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint",
                "AtomicCopyBufferUint64", "ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1",
                "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1", "BuildRaytracingAS",
                "EmitRaytracingASPostbuildInfo", "CopyRaytracingAS", "DispatchRays", "InitializeMetaCommand",
                "ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1",
                "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame",
                "ResolveEncoderOutputMetadata", "Barrier", "BeginCommandList", "DispatchGraph", "SetProgram"};
            return unsigned(op) < std::size(names) ? names[op] : "Other";
        }

        const char* AllocationName(D3D12_DRED_ALLOCATION_TYPE type)
        {
            switch (type)
            {
            case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "queue";
            case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command_allocator";
            case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline";
            case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command_list";
            case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "fence";
            case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor_heap";
            case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
            case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "query_heap";
            case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
            default: return "other";
            }
        }

        std::string Name(const char* narrow, const wchar_t* wide)
        {
            if (narrow && *narrow) return narrow;
            if (!wide || !*wide) return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (size <= 1) return {};
            std::string text(size_t(size - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr, nullptr);
            return text;
        }

        void LogAllocations(const char* kind, const D3D12_DRED_ALLOCATION_NODE1* node)
        {
            unsigned count = 0;
            for (; node; node = node->pNext, ++count)
                if (count < 12)
                    LOG_ERROR("dred: page fault {} type={} name=\"{}\" object={}", kind,
                        AllocationName(node->AllocationType), Name(node->ObjectNameA, node->ObjectNameW),
                        static_cast<const void*>(node->pObject));
            if (count > 12) LOG_ERROR("dred: page fault {}: {} more", kind, count - 12);
        }

        const char* DeviceStateName(int state)
        {
            switch (state)
            {
            case 3: return "hung";
            case 6: return "fault";
            case 7: return "page_fault";
            default: return "unknown";
            }
        }
    }

    void ConfigureBeforeDeviceCreation(bool debugLog)
    {
        bool want = debugLog;
        if (const char* forced = getenv("LO_D3D12_DRED"); forced && *forced)
            want = forced[0] != '0';
        if (!want) return;
        ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
        const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
        if (FAILED(hr) || !settings)
        {
            LOG_WARNING("video: D3D12 crash diagnostics unavailable hr={:#x}", uint32_t(hr));
            return;
        }
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->Release();
        g_enabled.store(true, std::memory_order_release);
        LOG_NOTICE("video: D3D12 crash diagnostics on (DRED breadcrumbs and page faults)");
    }

    bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

    void LogRemovedDevice(ID3D12Device* device)
    {
        if (!device || !Enabled()) return;
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (SUCCEEDED(removed) || g_logged.exchange(true)) return;

        ID3D12DeviceRemovedExtendedData1* dred = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred)
        {
            LOG_ERROR("dred: no data interface on the removed device (reason {:#x})", uint32_t(removed));
            return;
        }
        int state = 0;
        ID3D12DeviceRemovedExtendedData2* dred2 = nullptr;
        if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&dred2))) && dred2)
        {
            state = int(dred2->GetDeviceState());
            dred2->Release();
        }
        LOG_ERROR("dred: device removed reason={:#x} state={}", uint32_t(removed), DeviceStateName(state));

        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA)
        {
            LOG_ERROR("dred: page fault va={:#x}", uint64_t(fault.PageFaultVA));
            LogAllocations("existing", fault.pHeadExistingAllocationNode);
            LogAllocations("recently_freed", fault.pHeadRecentFreedAllocationNode);
        }
        else
            LOG_ERROR("dred: no page fault reported");

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
        if (FAILED(dred->GetAutoBreadcrumbsOutput1(&crumbs)))
        {
            LOG_ERROR("dred: no breadcrumbs");
            dred->Release();
            return;
        }
        // Lists the GPU finished are not the cause; print only unfinished ones
        // with the ops around the first that did not complete.
        constexpr uint32_t kRing = 65536;
        unsigned finished = 0, unfinished = 0;
        for (auto* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
        {
            const uint32_t total = node->BreadcrumbCount;
            const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (done >= total) { ++finished; continue; }
            if (++unfinished > 8) continue;
            std::string ops;
            const uint32_t first = done > 4 ? done - 4 : 0;
            const uint32_t last = (std::min)(total, done + 5);
            for (uint32_t i = first; i < last && node->pCommandHistory; ++i)
            {
                if (total - i > kRing) continue;
                ops += i == done ? " >" : " ";
                ops += OpName(node->pCommandHistory[i % kRing]);
            }
            LOG_ERROR("dred: unfinished queue=\"{}\" list=\"{}\" completed {} of {} ops:{}",
                Name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
                Name(node->pCommandListDebugNameA, node->pCommandListDebugNameW), done, total, ops);
        }
        LOG_ERROR("dred: {} unfinished and {} finished command lists", unfinished, finished);
        dred->Release();
    }
}
#endif
