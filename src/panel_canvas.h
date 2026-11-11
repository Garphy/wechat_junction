#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <span>

constexpr COLORREF canvasKey = RGB(2, 1, 2);
constexpr std::uint32_t canvasBackground = 0x01000000;

class PanelCanvas {
public:
    PanelCanvas() = default;
    PanelCanvas(const PanelCanvas&) = delete;
    PanelCanvas& operator=(const PanelCanvas&) = delete;
    ~PanelCanvas() { Reset(); }

    void Reset() {
        if (dc_) DeleteDC(dc_);
        if (bitmap_) DeleteObject(bitmap_);
        dc_ = nullptr;
        bitmap_ = nullptr;
        pixels_ = nullptr;
        size_ = {};
    }

    HDC Prepare(LONG width, LONG height, std::uint32_t background = canvasKey) {
        if (width <= 0 || height <= 0) return nullptr;
        if (!dc_) dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) return nullptr;
        if (!bitmap_ || size_.cx != width || size_.cy != height) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HBITMAP next = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!next) return nullptr;
            auto previous = SelectObject(dc_, next);
            if (!previous || previous == HGDI_ERROR) {
                DeleteObject(next);
                return nullptr;
            }
            if (bitmap_) DeleteObject(bitmap_);
            bitmap_ = next;
            pixels_ = static_cast<std::uint32_t*>(bits);
            size_ = {width, height};
        }
        GdiFlush();
        std::fill_n(pixels_, PixelCount(), background);
        return dc_;
    }

    std::span<const std::uint32_t> Pixels() const {
        GdiFlush();
        return {pixels_, PixelCount()};
    }

    void NormalizeAlpha() {
        GdiFlush();
        for (size_t i = 0; i < PixelCount(); ++i) {
            auto color = pixels_[i] & 0x00FFFFFF;
            // Alpha zero is click-through. Alpha one is visually negligible and keeps the hit area.
            pixels_[i] = color == canvasKey ? canvasBackground : color | 0xFF000000;
        }
    }

    void CompositeBackground(RECT area, RECT clip, std::span<const std::uint32_t> overlay) {
        RECT bounds{0, 0, size_.cx, size_.cy};
        RECT visible{};
        if (!IntersectRect(&visible, &area, &clip) || !IntersectRect(&visible, &visible, &bounds)) return;
        size_t width = area.right - area.left;
        size_t height = area.bottom - area.top;
        if (overlay.size() != width * height) return;
        for (LONG y = visible.top; y < visible.bottom; ++y) {
            auto* destination = pixels_ + size_t(y) * size_.cx;
            const auto* source = overlay.data() + size_t(y - area.top) * width;
            for (LONG x = visible.left; x < visible.right; ++x) {
                auto color = source[x - area.left];
                if (destination[x] == canvasBackground && (color >> 24) != 0) destination[x] = color;
            }
        }
    }

    void Present(HWND window) {
        POINT source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(window, nullptr, nullptr, &size_, dc_, &source, 0, &blend, ULW_ALPHA);
    }

private:
    size_t PixelCount() const { return size_t(size_.cx) * size_.cy; }
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
    SIZE size_{};
};
