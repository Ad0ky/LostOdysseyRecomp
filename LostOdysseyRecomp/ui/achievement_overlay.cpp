#include <stdafx.h>
#include "achievement_overlay.h"
#include "achievement_toast.h"
#include "text_raster.h"
#include <achievements/achievements.h>
#include <apu/audio.h>
#include <kernel/io/file_system.h>
#include <os/logger.h>
#include <os/user_paths.h>
#include <settings/config.h>
#include <settings/menu_assets.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace ui::achievement_overlay {
namespace {
namespace menu_assets = settings::menu_assets;

using text_raster::DrawText;
using text_raster::Raster;
using text_raster::UiLanguageFor;

// achievement.wav next to settings.ini replaces the built-in chime.
std::shared_ptr<const std::vector<float>> Chime() {
    static const auto chime = [] {
        std::vector<float> samples;
        const auto path = os::user_paths::SettingsPath().parent_path() / "achievement.wav";
        SDL_AudioSpec spec{};
        Uint8* data = nullptr;
        Uint32 length = 0;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) && SDL_LoadWAV(path.string().c_str(), &spec, &data, &length)) {
            const SDL_AudioSpec mono{SDL_AUDIO_F32, 1, 48000};
            Uint8* converted = nullptr;
            int convertedLength = 0;
            if (SDL_ConvertAudioSamples(&spec, data, int(length), &mono, &converted, &convertedLength)) {
                samples.assign(reinterpret_cast<const float*>(converted), reinterpret_cast<const float*>(converted) + convertedLength / 4);
                SDL_free(converted);
                LOG_INFO("achievements: chime from {} ({:.2f} s)", path.string(), samples.size() / 48000.0);
            }
            SDL_free(data);
        }
        if (samples.empty()) samples = ui::achievement_toast::SynthesizeChime(48000);
        return std::make_shared<const std::vector<float>>(std::move(samples));
    }();
    return chime;
}

struct Toast {
    uint16_t id = 0;
    std::optional<achievements::Info> info;
    Raster icon, title, detail;
    float scale = 0;
    ui::achievement_toast::Content content;
    double clock = 0;
    std::chrono::steady_clock::time_point last;
};
std::optional<Toast> gToast;
Image gImage;
uint64_t gRevision = 0;

void Prepare(Toast& toast, float scale) {
    toast.scale = scale;
    const auto assets = menu_assets::Cached(FileSystem::GetGameRoot(), UiLanguageFor(settings::GameLanguage()));
    toast.title = DrawText(assets.get(), achievements::UnlockedHeading(), 21.0 * scale);
    toast.detail = DrawText(assets.get(), fmt::format("{}G - {}", toast.info->score, toast.info->name), 16.0 * scale);
    auto& c = toast.content;
    c.revision = ++gRevision;
    c.icon = toast.icon.pixels.empty() ? nullptr : toast.icon.pixels.data();
    c.iconWidth = toast.icon.width;
    c.iconHeight = toast.icon.height;
    c.title = toast.title.pixels.data();
    c.titleWidth = toast.title.width;
    c.titleHeight = toast.title.height;
    c.detail = toast.detail.pixels.data();
    c.detailWidth = toast.detail.width;
    c.detailHeight = toast.detail.height;
}
}

const Image* Update(uint32_t outputWidth, uint32_t outputHeight) {
    if (!outputWidth || !outputHeight) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (!gToast) {
        const auto id = achievements::NextAnnouncement();
        if (!id) return nullptr;
        const auto config = settings::GetConfig();
        if (!config.achievementNotifications) return nullptr;
        Toast toast;
        toast.id = *id;
        toast.info = achievements::Find(*id);
        if (!toast.info) return nullptr;
        auto icon = achievements::IconOf(toast.id);
        toast.icon = {std::move(icon.pixels), icon.width, icon.height};
        toast.last = now;
        LOG_INFO("achievements: toast for {} ({}x{} icon)", toast.id, toast.icon.width, toast.icon.height);
        gToast = std::move(toast);
        if (config.achievementSound) apu::PlayUiSound(Chime());
    } else {
        // Frames that were not presented (menus, loads) do not count.
        gToast->clock += std::min(std::chrono::duration<double>(now - gToast->last).count(), 0.05);
        gToast->last = now;
    }
    const float scale = std::max(0.5f, std::min(outputWidth / 1280.0f, outputHeight / 720.0f));
    if (scale != gToast->scale) Prepare(*gToast, scale);
    if (gToast->clock >= ui::achievement_toast::Duration()) {
        gToast.reset();
        return nullptr;
    }
    auto frame = ui::achievement_toast::Render(gToast->content, gToast->clock, scale);
    if (!frame.visible || !frame.width || !frame.height) return nullptr;
    gImage.pixels = std::move(frame.pixels);
    gImage.width = frame.width;
    gImage.height = frame.height;
    // Bottom centre, inside the title-safe area like the console's own toast.
    const int anchorX = int(outputWidth / 2), anchorY = int(outputHeight * 0.88);
    gImage.x = std::clamp(anchorX + frame.offsetX, 0, std::max(0, int(outputWidth) - int(frame.width)));
    gImage.y = std::clamp(anchorY + frame.offsetY, 0, std::max(0, int(outputHeight) - int(frame.height)));
    if (gImage.width > outputWidth || gImage.height > outputHeight) return nullptr;
    return &gImage;
}
}
