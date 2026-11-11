#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace placement {

inline RECT Fit(RECT bounds, const RECT& work) {
    LONG width = std::min(bounds.right - bounds.left, work.right - work.left);
    LONG height = std::min(bounds.bottom - bounds.top, work.bottom - work.top);
    LONG x = std::clamp(bounds.left, work.left, work.right - width);
    LONG y = std::clamp(bounds.top, work.top, work.bottom - height);
    return {x, y, x + width, y + height};
}

inline RECT ScaleToWorkArea(const RECT& bounds, POINT savedOrigin, UINT savedDpi, const RECT& work, UINT dpi) {
    using Pixels = std::int64_t;
    auto scale = [=](Pixels value) {
        return (value * dpi + (value < 0 ? -1 : 1) * (static_cast<Pixels>(savedDpi) / 2)) / savedDpi;
    };
    Pixels width = std::clamp<Pixels>(scale(Pixels(bounds.right) - bounds.left), 1, work.right - work.left);
    Pixels height = std::clamp<Pixels>(scale(Pixels(bounds.bottom) - bounds.top), 1, work.bottom - work.top);
    auto x = static_cast<LONG>(std::clamp<Pixels>(work.left + scale(Pixels(bounds.left) - savedOrigin.x),
        work.left, work.right - width));
    auto y = static_cast<LONG>(std::clamp<Pixels>(work.top + scale(Pixels(bounds.top) - savedOrigin.y),
        work.top, work.bottom - height));
    return {x, y, x + static_cast<LONG>(width), y + static_cast<LONG>(height)};
}

}
