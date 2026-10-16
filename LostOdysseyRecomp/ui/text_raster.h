#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace settings::menu_assets { struct Assets; }

// One line of host overlay text in the game's menu font, for the overlays
// drawn over game frames (achievement toast, performance overlay).
namespace ui::text_raster {
struct Raster {
    std::vector<uint32_t> pixels; // straight-alpha RGBA8 (R in the low byte)
    uint32_t width = 0, height = 0;
    double advance = 0; // pen advance in output pixels, without the 1-pixel margins
};
// The menu font of the UI language that shares the game language's script.
uint32_t UiLanguageFor(uint32_t gameLanguage);
// `utf8` in `face` (0xBBGGRR) with the game font's dark edge, `height` output
// pixels tall. A string the game fonts cannot cover comes from Unifont.
// fallbackFirst tries the fallback font (Abc) before the body font, so a run
// split into characters keeps one font.
Raster DrawText(const settings::menu_assets::Assets* assets, const std::string& utf8, double height,
    uint32_t face = 0xFFFFFFu, bool fallbackFirst = false);
}
