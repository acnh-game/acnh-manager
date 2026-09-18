#pragma once

/* Header tab geometry: "L status" / "R details".

   The pill and its touch target have to come from the same numbers.  The first version of this
   header drew the tabs but never registered them as actions, so they could not be tapped at
   all.  Keeping the layout here (pure, no libnx) lets the host tests pin what actually
   matters: both tabs are hit-testable where they are drawn, and the two targets never
   overlap, so a tap can never be ambiguous. */

#include "ui/action.hpp"

namespace acnh_manager::ui {

/* Right-hand inset of the header contents; app.cpp asserts its drawing constant matches. */
constexpr int kHeaderMargin = 40;
/* Height of the bar the tabs live in (they are drawn in its upper part). */
constexpr int kHeaderBarHeight = 96;
constexpr int kHeaderTabBadge = 40;
constexpr int kHeaderTabBadgeGap = 14;  /* badge <-> label */
constexpr int kHeaderTabSpacing = 26;   /* between the two tab groups */
constexpr int kHeaderTabPadX = 8;       /* touch slop around the drawn group */
constexpr int kHeaderTabPadY = 8;
constexpr int kHeaderTabTop = 32;       /* top of the badge */

struct HeaderTabLayout {
    int badge_x[2];  /* left edge of the Ⓛ / Ⓡ circle */
    int label_x[2];  /* left edge of the label text */
    Rect hit[2];     /* whole touch target for each tab */
};

/* Lay both tabs out from the right edge; index 0 is the left-hand tab (status).  The caller
   passes the measured label widths because only the font knows them. */
inline HeaderTabLayout LayoutHeaderTabs(int surface_width, const int label_width[2]) {
    HeaderTabLayout layout{};
    int x = surface_width - kHeaderMargin;
    for (int i = 1; i >= 0; --i) {
        x -= label_width[i];
        layout.label_x[i] = x;
        x -= kHeaderTabBadge + kHeaderTabBadgeGap;
        layout.badge_x[i] = x;
        x -= kHeaderTabSpacing;
    }
    for (int i = 0; i < 2; ++i) {
        const int left = layout.badge_x[i] - kHeaderTabPadX;
        const int right = layout.label_x[i] + label_width[i] + kHeaderTabPadX;
        layout.hit[i] = Rect{left, kHeaderTabTop - kHeaderTabPadY, right - left,
                             kHeaderTabBadge + 2 * kHeaderTabPadY};
    }
    return layout;
}

}  // namespace acnh_manager::ui
