#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <vector>

class SelectionStyle {
public:
    void Reset();
    std::span<const std::uint32_t> Pixels(HWND list, LONG width, LONG height, UINT dpi, bool focused, bool highContrast);

private:
    struct Cache {
        LONG width = 0, height = 0;
        UINT dpi = 0;
        bool highContrast = false;
        std::vector<std::uint32_t> pixels;
    };
    Cache caches_[2];
};
