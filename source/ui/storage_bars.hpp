#pragma once

/* The two storage blocks in the header of the game-install page: microSD on the left, the
   console's own memory on the right.  They show how full each storage is, and they are *also* the
   control that picks where the run writes (design section 8.1) -- the chosen one carries a panel
   behind it, the other keeps its numbers but dims, so the pair still compares.

   The geometry lives here (pure C++, no libnx) for the same reason the header tabs' does: what the
   drawing shows and what a tap hits have to come from one set of numbers.  A block whose hit box
   does not match its pixels is worse than no control at all -- the player taps the storage they
   mean and gets the other one -- and that is exactly the class of bug the host tests below pin. */

#include "ui/action.hpp"
#include "ui/header_tabs.hpp"

namespace acnh_manager::ui {

/* Index 0 is microSD, index 1 the console's own memory: the order L / R walks through them, and
   the order the pair is drawn in (left to right). */
constexpr int kStorageBarCount = 2;
constexpr int kStorageBarHeight = 8;
/* Between the label and the blocks, and between the two panels. */
constexpr int kStorageBlockGap = 32;
/* A control group fills the header band: a panel from kStoragePanelTop to its bottom, holding the
   key badge, the caption and the bar.  The panel is deliberately tall and only slightly rounded --
   a panel that wraps the whole group reads as "this is one control, and it is the chosen one",
   while a tight outline around two lines of text reads as decoration. */
constexpr int kStoragePanelTop = 12;
constexpr int kStoragePanelHeight = 72;
constexpr int kStoragePanelPadX = 14;
constexpr int kStoragePanelRadius = 8;
/* Inside it: Ⓛ badge, caption, bar -- the two text lines centred as a block. */
constexpr int kStorageBadge = 34;
constexpr int kStorageBadgeGap = 10;
constexpr int kStorageCaptionY = 29;
constexpr int kStorageBarY = 59;
constexpr int kStoragePanelCenterY = kStoragePanelTop + kStoragePanelHeight / 2;

struct StorageBarsLayout {
    int label_x;                     /* left edge of the fixed "install to:" label */
    int badge_x[kStorageBarCount];   /* left edge of the key badge (L / R) inside each panel */
    int badge_y;
    int caption_x[kStorageBarCount]; /* left edge of the caption; the bar shares it and its width */
    int block_w[kStorageBarCount];   /* caption width: the two lines read as one block */
    Rect panel[kStorageBarCount];    /* the whole control group: badge + caption + bar + padding */
};

/* Lay the label and the two blocks out from the right edge inwards.  The caller passes the measured
   widths, because only the font knows them. */
inline StorageBarsLayout LayoutStorageBars(int surface_width,
                                           int label_w,
                                           const int text_w[kStorageBarCount]) {
    StorageBarsLayout layout{};
    int x_right = surface_width - kHeaderMargin;
    for (int i = kStorageBarCount - 1; i >= 0; --i) {
        layout.block_w[i] = text_w[i];
        layout.caption_x[i] = x_right - kStoragePanelPadX - text_w[i];
        const int panel_w = kStoragePanelPadX + kStorageBadge + kStorageBadgeGap + text_w[i] +
                            kStoragePanelPadX;
        layout.panel[i] = Rect{x_right - panel_w, kStoragePanelTop, panel_w, kStoragePanelHeight};
        layout.badge_x[i] = layout.panel[i].x + kStoragePanelPadX;
        layout.badge_y = kStoragePanelCenterY - kStorageBadge / 2;
        x_right = layout.panel[i].x - kStorageBlockGap;
    }
    /* The label goes immediately left of the pair, so the whole line reads
       "install to:  Ⓛ [microSD]  Ⓡ [system]". */
    layout.label_x = x_right - label_w;
    return layout;
}

}  // namespace acnh_manager::ui
