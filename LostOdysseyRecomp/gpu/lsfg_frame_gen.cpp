/* SPDX-License-Identifier: GPL-3.0-or-later */
// Same-device LSFG presentation, adapted from NetherSX2_nx's
// source/lsfg/lsfg_bridge.cpp (GPL-3.0-or-later, f084dc1). Every presenter
// Vulkan handle is borrowed; LSFG owns its private images, pipelines and
// synchronization. See lsfg_frame_gen.h.
#include <stdafx.h>
#include "lsfg_frame_gen.h"

#if LO_PLATFORM_SWITCH && defined(LO_GPU_PLUME)

#include <os/logger.h>
#include <plume_vulkan.h>

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                  VkImageLayout oldLayout, VkImageLayout newLayout)
{
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = sourceAccess,
        .dstAccessMask = destinationAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

struct FrameSlot
{
    explicit FrameSlot(const vk::Vulkan& vulkan)
        : frame(vulkan), restore(vulkan), completion(vulkan), acquired(vulkan), generated(vulkan), original(vulkan)
    {
    }

    vk::CommandBuffer frame;
    vk::CommandBuffer restore;
    vk::Fence completion;
    vk::Semaphore acquired;
    vk::Semaphore generated;
    vk::Semaphore original;
    bool pending{};
};

float EnvFloat(const char* name, float fallback)
{
    const char* value = std::getenv(name);
    return value && *value ? std::strtof(value, nullptr) : fallback;
}

std::filesystem::path DataRoot()
{
#ifdef LO_SWITCH_DATA_ROOT
    return LO_SWITCH_DATA_ROOT;
#else
    return "/switch/LostOdysseyRecomp";
#endif
}

// One frame-generation session for one swapchain.
class Runtime
{
public:
    Runtime(plume::VulkanSwapChain& swapChain)
        : swapchain_(swapChain.vk), extent_{swapChain.width, swapChain.height}
    {
        auto* queue = swapChain.commandQueue;
        auto* device = queue->device;
        for (auto& texture : swapChain.textures) images_.push_back(texture.vk);
        if (images_.empty() || !extent_.width || !extent_.height) throw std::runtime_error("empty swapchain");
        if (!device->timelineSemaphoresSupported) throw std::runtime_error("timeline semaphores unavailable");

        float flowScale = EnvFloat("LO_FRAMEGEN_FLOW", 0.25f);
        if (!std::isfinite(flowScale) || flowScale < 0.125f || flowScale > 0.5f) flowScale = 0.25f;
        const char* perfValue = std::getenv("LO_FRAMEGEN_PERF");
        const bool performance = !(perfValue && perfValue[0] == '0');

        const lsfgvk::backend::BorrowedDevice borrowed{
            .instance = device->renderInterface->instance,
            .physicalDevice = device->physicalDevice,
            .device = device->vk,
            .queueFamilyIndex = queue->familyIndex,
            .queue = queue->queue->vk,
            .getInstanceProcAddr = vkGetInstanceProcAddr,
            .pipelineCachePath = DataRoot() / "cache" / "lsfg-vk-pipeline-cache.bin",
        };
        backend_ = std::make_unique<lsfgvk::backend::Instance>(borrowed, DataRoot() / "lsfg" / "Lossless.dll", false);
        context_ = &backend_->openLocalContext(extent_.width, extent_.height, false, 1.0f / flowScale, performance, 1,
                                               VK_QUEUE_FAMILY_IGNORED);
        vulkan_ = &backend_->vulkan();
        if (!vulkan_->df().AcquireNextImageKHR || !vulkan_->df().QueuePresentKHR)
            throw std::runtime_error("swapchain entry points are unavailable");
        const size_t slotCount = std::max<size_t>(images_.size(), 3);
        slots_.reserve(slotCount);
        for (size_t i = 0; i < slotCount; ++i) slots_.emplace_back(*vulkan_);
        LOG_NOTICE("framegen: LSFG ready {}x{} ({} swapchain images, flow scale {}, {} mode)", extent_.width,
                   extent_.height, images_.size(), flowScale, performance ? "performance" : "quality");
    }

    ~Runtime()
    {
        if (vulkan_ && vulkan_->df().DeviceWaitIdle) (void)vulkan_->df().DeviceWaitIdle(vulkan_->dev());
    }

    bool Matches(const plume::VulkanSwapChain& swapChain) const
    {
        return swapChain.vk == swapchain_ && swapChain.width == extent_.width && swapChain.height == extent_.height &&
               swapChain.textures.size() == images_.size();
    }

    // Generated frame, then the real one. Returns the real frame's result.
    VkResult Present(VkQueue queue, VkSemaphore wait, uint32_t imageIndex)
    {
        if (imageIndex >= images_.size()) throw std::runtime_error("swapchain image index out of range");
        FrameSlot& slot = PrepareSlot();
        // Source images alternate; the previous frame stays in the other one.
        const size_t sourceIndex = size_t((realFrame_ + 1) & 1);
        const VkImage source = backend_->sourceImage(*context_, sourceIndex);
        std::vector<VkSemaphore> waits;
        if (wait) waits.push_back(wait);

        slot.frame.begin(*vulkan_);
        RecordCapture(slot.frame, images_[imageIndex], source, sourceInitialized_[sourceIndex]);

        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.swapchainCount = 1;
        info.pSwapchains = &swapchain_;

        if (realFrame_ == 0)
        {
            // Nothing to interpolate from yet: keep the frame and show it.
            slot.frame.end(*vulkan_);
            slot.frame.submit(*vulkan_, std::move(waits), VK_NULL_HANDLE, 0, {slot.original.handle()},
                              VK_NULL_HANDLE, 0, slot.completion.handle());
            sourceInitialized_[sourceIndex] = true;
            slot.pending = true;
            ++realFrame_;
            return PresentImage(queue, info, imageIndex, slot.original.handle());
        }

        backend_->recordFrame(*context_, slot.frame);
        // VI allows one acquired image: present the generated frame in this
        // image, reacquire, then copy the kept real frame into the next one.
        RecordCopyToSwapchain(slot.frame, backend_->destinationImage(*context_, 0), images_[imageIndex], true);
        slot.frame.end(*vulkan_);
        slot.frame.submit(*vulkan_, std::move(waits), VK_NULL_HANDLE, 0, {slot.generated.handle()}, VK_NULL_HANDLE,
                          0);
        sourceInitialized_[sourceIndex] = true;

        const VkResult generated = PresentImage(queue, info, imageIndex, slot.generated.handle());
        if (generated != VK_SUCCESS && generated != VK_SUBOPTIMAL_KHR) return generated;

        uint32_t realIndex = 0;
        const VkResult acquire = vulkan_->df().AcquireNextImageKHR(vulkan_->dev(), swapchain_, UINT64_MAX,
                                                                   slot.acquired.handle(), VK_NULL_HANDLE, &realIndex);
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) return acquire;
        if (realIndex >= images_.size()) throw std::runtime_error("reacquired image index out of range");

        slot.restore.begin(*vulkan_);
        RecordCopyToSwapchain(slot.restore, source, images_[realIndex], false);
        slot.restore.end(*vulkan_);
        slot.restore.submit(*vulkan_, {slot.acquired.handle()}, VK_NULL_HANDLE, 0, {slot.original.handle()},
                            VK_NULL_HANDLE, 0, slot.completion.handle());
        slot.pending = true;
        ++realFrame_;
        return PresentImage(queue, info, realIndex, slot.original.handle());
    }

private:
    FrameSlot& PrepareSlot()
    {
        FrameSlot& slot = slots_.at(slotCursor_++ % slots_.size());
        if (slot.pending)
        {
            if (!slot.completion.wait(*vulkan_)) throw std::runtime_error("timeout waiting for an LSFG frame slot");
            slot.pending = false;
        }
        slot.completion.reset(*vulkan_);
        return slot;
    }

    void RecordCapture(const vk::CommandBuffer& command, VkImage swapchainImage, VkImage source,
                       bool sourceInitialized) const
    {
        command.copyImage(*vulkan_,
            {ImageBarrier(swapchainImage, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
             ImageBarrier(source, sourceInitialized ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          sourceInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)},
            {swapchainImage, source}, extent_,
            {ImageBarrier(swapchainImage, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
             ImageBarrier(source, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)});
    }

    // generatedOutput: the LSFG destination image (written by compute) to
    // the presented image; otherwise the kept real frame to a fresh image.
    void RecordCopyToSwapchain(const vk::CommandBuffer& command, VkImage from, VkImage swapchainImage,
                               bool generatedOutput) const
    {
        const VkAccessFlags fromAccess = generatedOutput ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        command.copyImage(*vulkan_,
            {ImageBarrier(from, fromAccess, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
             ImageBarrier(swapchainImage, generatedOutput ? VK_ACCESS_MEMORY_READ_BIT : 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                          generatedOutput ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)},
            {from, swapchainImage}, extent_,
            {ImageBarrier(from, VK_ACCESS_TRANSFER_READ_BIT, fromAccess, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_GENERAL),
             ImageBarrier(swapchainImage, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)});
    }

    VkResult PresentImage(VkQueue queue, VkPresentInfoKHR info, uint32_t imageIndex, VkSemaphore wait) const
    {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &wait;
        info.pImageIndices = &imageIndex;
        return vulkan_->df().QueuePresentKHR(queue, &info);
    }

    VkSwapchainKHR swapchain_{};
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::unique_ptr<lsfgvk::backend::Instance> backend_;
    lsfgvk::backend::Context* context_{};
    const vk::Vulkan* vulkan_{};
    std::vector<FrameSlot> slots_;
    std::array<bool, 2> sourceInitialized_{false, false};
    uint64_t realFrame_{};
    size_t slotCursor_{};
};

std::unique_ptr<Runtime> g_runtime;
std::atomic<bool> g_failed{false};
// Presenter-side cost of a frame-generated present, for the periodic log.
uint64_t g_frames = 0;
double g_presentMs = 0, g_maxPresentMs = 0;

void Fail(const char* what, const std::string& detail)
{
    g_runtime.reset();
    g_failed.store(true, std::memory_order_relaxed);
    LOG_ERROR("framegen: {} ({}); frame generation off for this session", what, detail);
}
}

namespace gpu::lsfg
{
bool Enabled()
{
    static const bool requested = [] {
        const char* value = std::getenv("LO_FRAMEGEN");
        return value && value[0] == '1';
    }();
    return requested && !g_failed.load(std::memory_order_relaxed);
}

bool Present(plume::RenderSwapChain* swapChain, uint32_t imageIndex, plume::RenderCommandSemaphore* waitSemaphore,
             bool& presented)
{
    if (!Enabled() || !swapChain) return false;
    auto& vulkanSwapChain = *static_cast<plume::VulkanSwapChain*>(swapChain);
    if (vulkanSwapChain.vk == VK_NULL_HANDLE) return false;
    // The generated frame is copied byte for byte: any 4-byte UNORM swapchain works.
    VkFormat format = vulkanSwapChain.textures.empty() ? VK_FORMAT_UNDEFINED : vulkanSwapChain.textures[0].imageFormat;
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM)
    {
        Fail("unsupported swapchain format", std::to_string(int(format)));
        return false;
    }
    auto* queue = vulkanSwapChain.commandQueue;
    // LSFG submits and presents on the presenter's queue: hold its lock.
    std::scoped_lock lock(*queue->queue->mutex);
    try
    {
        if (g_runtime && !g_runtime->Matches(vulkanSwapChain)) g_runtime.reset();
        if (!g_runtime)
        {
            if (!std::filesystem::exists(DataRoot() / "lsfg" / "Lossless.dll"))
            {
                Fail("Lossless.dll not found", (DataRoot() / "lsfg" / "Lossless.dll").string());
                return false;
            }
            std::error_code ignored;
            std::filesystem::create_directories(DataRoot() / "cache", ignored);
            g_runtime = std::make_unique<Runtime>(vulkanSwapChain);
        }
    }
    catch (const std::exception& error)
    {
        Fail("setup failed", error.what());
        return false; // nothing was presented: the caller presents normally
    }
    try
    {
        const auto start = std::chrono::steady_clock::now();
        VkSemaphore wait = waitSemaphore ? static_cast<plume::VulkanCommandSemaphore*>(waitSemaphore)->vk : VK_NULL_HANDLE;
        const VkResult result = g_runtime->Present(queue->queue->vk, wait, imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) vulkanSwapChain.surfaceOutOfDate = true;
        presented = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        g_presentMs += ms;
        g_maxPresentMs = std::max(g_maxPresentMs, ms);
        if (++g_frames % 300 == 0)
        {
            LOG_INFO("framegen: {} frames, present+generate {:.2f} ms avg, {:.2f} ms max (CPU wait incl. vsync)",
                     g_frames, g_presentMs / 300, g_maxPresentMs);
            g_presentMs = g_maxPresentMs = 0;
        }
        if (!presented) LOG_WARNING("framegen: present returned {}", int(result));
    }
    catch (const std::exception& error)
    {
        // The image may already be queued: report a failed present rather
        // than letting the caller present it a second time.
        Fail("present failed", error.what());
        presented = false;
    }
    return true;
}

void Reset()
{
    if (!g_runtime) return;
    g_runtime.reset();
}
}

#else

namespace gpu::lsfg
{
bool Enabled() { return false; }
bool Present(plume::RenderSwapChain*, uint32_t, plume::RenderCommandSemaphore*, bool&) { return false; }
void Reset() {}
}

#endif
