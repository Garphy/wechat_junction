#pragma once

#include <windows.h>
#include <commctrl.h>

#include <algorithm>

class ListRowSizing {
public:
    ListRowSizing() = default;
    ListRowSizing(const ListRowSizing&) = delete;
    ListRowSizing& operator=(const ListRowSizing&) = delete;
    ~ListRowSizing() { if (sizing_) ImageList_Destroy(sizing_); }

    bool HasPlaceholder() const { return sizing_ != nullptr; }
    int IconSize() const { return iconSize_; }
    int IconOffset() const { return iconOffset_; }

    void Update(HWND list, HIMAGELIST fileImages, int iconSize, int padding, int iconGap) {
        int width = 0, height = 0;
        if (!fileImages || !ImageList_GetIconSize(fileImages, &width, &height)) return;

        HIMAGELIST baseline = nullptr;
        if (width != iconSize || height != iconSize) {
            baseline = Create(iconSize, iconSize);
            if (!baseline) return;
        }
        ListView_SetImageList(list, baseline ? baseline : fileImages, LVSIL_SMALL);
        int baseRowHeight = RowHeight(list);
        int targetRowHeight = baseRowHeight + padding;
        int sizingHeight = std::max(iconSize, baseRowHeight) + padding;
        int sizingWidth = iconSize + iconGap;
        auto sizing = Create(sizingWidth, sizingHeight);
        if (!sizing) {
            ListView_SetImageList(list, sizing_ ? sizing_ : fileImages, LVSIL_SMALL);
            if (baseline) ImageList_Destroy(baseline);
            if (!sizing_) iconSize_ = width;
            return;
        }

        ListView_SetImageList(list, sizing, LVSIL_SMALL);
        if (baseline) ImageList_Destroy(baseline);
        // The font can set a higher floor than the icon, and list themes can add their own padding.
        int actualRowHeight = RowHeight(list);
        int correctedHeight = sizingHeight + targetRowHeight - actualRowHeight;
        if (baseRowHeight > 0 && actualRowHeight > 0 && correctedHeight >= iconSize && correctedHeight != sizingHeight) {
            if (auto corrected = Create(sizingWidth, correctedHeight)) {
                ListView_SetImageList(list, corrected, LVSIL_SMALL);
                ImageList_Destroy(sizing);
                sizing = corrected;
                sizingHeight = correctedHeight;
            }
        }
        if (sizing_) ImageList_Destroy(sizing_);
        sizing_ = sizing;
        iconSize_ = iconSize;
        iconOffset_ = (sizingHeight - iconSize) / 2;
    }

private:
    static int RowHeight(HWND list) {
        return HIWORD(ListView_ApproximateViewRect(list, -1, -1, 2)) -
            HIWORD(ListView_ApproximateViewRect(list, -1, -1, 1));
    }

    static HIMAGELIST Create(int width, int height) {
        auto image = ImageList_Create(width, height, ILC_COLOR32 | ILC_MASK, 1, 0);
        if (image && !ImageList_SetImageCount(image, 1)) {
            ImageList_Destroy(image);
            image = nullptr;
        }
        return image;
    }

    HIMAGELIST sizing_ = nullptr;
    int iconSize_ = 0;
    int iconOffset_ = 0;
};
