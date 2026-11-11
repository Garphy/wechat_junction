#include "selection_style.h"

#include "panel_canvas.h"

#include <uxtheme.h>
#include <vsstyle.h>

namespace {
unsigned Channel(std::uint32_t color, unsigned shift) {
    return (color >> shift) & 0xFF;
}

std::uint32_t RecoverPixel(std::uint32_t black, std::uint32_t white, unsigned limit) {
    unsigned transmitted = 0;
    for (unsigned shift : {0u, 8u, 16u}) {
        unsigned dark = Channel(black, shift), light = Channel(white, shift);
        transmitted += light > dark ? light - dark : 0;
    }
    unsigned alpha = 255 - transmitted / 3;
    if (!alpha) return 0;
    unsigned capped = std::min(alpha, limit);
    auto result = std::uint32_t(capped) << 24;
    for (unsigned shift : {0u, 8u, 16u}) {
        result |= (std::min(alpha, Channel(black, shift)) * capped / alpha) << shift;
    }
    return result;
}

bool DrawSystemSelection(HWND list, LONG width, LONG height, bool focused, std::vector<std::uint32_t>& pixels) {
    if (!list) return false;
    PanelCanvas sample;
    HDC dc = sample.Prepare(width, height, 0);
    if (!dc) return false;
    RECT area{0, 0, width, height};
    HTHEME theme = OpenThemeData(list, L"Explorer::ListView");
    if (!theme) return false;
    int state = focused ? LISS_SELECTED : LISS_SELECTEDNOTFOCUS;
    bool drawn = SUCCEEDED(DrawThemeBackground(theme, dc, LVP_LISTITEM, state, &area, nullptr));
    if (drawn) {
        auto black = sample.Pixels();
        std::copy(black.begin(), black.end(), pixels.begin());
        sample.Prepare(width, height, 0x00FFFFFF);
        drawn = SUCCEEDED(DrawThemeBackground(theme, dc, LVP_LISTITEM, state, &area, nullptr));
    }
    CloseThemeData(theme);
    if (!drawn) return false;
    auto white = sample.Pixels();
    unsigned limit = focused ? 86 : 56;
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = RecoverPixel(pixels[i], white[i], limit);
    }
    return (pixels[size_t(height / 2) * width + width / 2] >> 24) >= 12;
}

bool InRoundedRect(LONG x, LONG y, LONG width, LONG height, LONG radius) {
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    LONG dx = std::max(0L, radius - 1 - std::min(x, width - 1 - x));
    LONG dy = std::max(0L, radius - 1 - std::min(y, height - 1 - y));
    return dx * dx + dy * dy < radius * radius || !radius;
}

std::uint32_t Tint(COLORREF color, unsigned white, unsigned alpha) {
    auto channel = [=](unsigned value) {
        return ((value * (255 - white) + 255 * white) / 255) * alpha / 255;
    };
    return (alpha << 24) | (channel(GetRValue(color)) << 16) |
        (channel(GetGValue(color)) << 8) | channel(GetBValue(color));
}

void DrawFallback(LONG width, LONG height, UINT dpi, bool focused, bool highContrast,
    std::vector<std::uint32_t>& pixels) {
    COLORREF highlight = GetSysColor(COLOR_HIGHLIGHT);
    if (highContrast) {
        std::fill(pixels.begin(), pixels.end(), Tint(highlight, 0, 255));
        return;
    }
    LONG radius = std::min({LONG(MulDiv(5, dpi, 96)), width / 2, height / 2});
    auto edge = Tint(highlight, 130, focused ? 110 : 75);
    for (LONG y = 0; y < height; ++y) {
        auto fade = unsigned(y * 32 / std::max(1L, height - 1));
        auto fill = Tint(highlight, 130 - fade * 2, (focused ? 82u : 54u) - fade);
        for (LONG x = 0; x < width; ++x) {
            auto& pixel = pixels[size_t(y) * width + x];
            pixel = 0;
            if (InRoundedRect(x, y, width, height, radius)) {
                pixel = InRoundedRect(x - 1, y - 1, width - 2, height - 2, std::max(0L, radius - 1)) ? fill : edge;
            }
        }
    }
}
}

void SelectionStyle::Reset() {
    for (auto& cache : caches_) cache = {};
}

std::span<const std::uint32_t> SelectionStyle::Pixels(HWND list, LONG width, LONG height, UINT dpi,
    bool focused, bool highContrast) {
    if (width <= 0 || height <= 0) return {};
    auto& cache = caches_[focused];
    if (cache.width != width || cache.height != height || cache.dpi != dpi || cache.highContrast != highContrast) {
        cache.pixels.resize(size_t(width) * height);
        if (highContrast || !DrawSystemSelection(list, width, height, focused, cache.pixels)) {
            DrawFallback(width, height, dpi, focused, highContrast, cache.pixels);
        }
        cache.width = width;
        cache.height = height;
        cache.dpi = dpi;
        cache.highContrast = highContrast;
    }
    return cache.pixels;
}
