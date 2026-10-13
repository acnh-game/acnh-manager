#pragma once

/* Header tab geometry: "Ⓛ status" / "details" / "+ game install" / "Ⓡ guide".

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
/* Four entries, laid out from the right edge: the game installer ("+"), the guide ("-"), the
   details page and the status page.  The two page tabs keep their place no matter how wide the
   other labels are, and the installer page is the one page that hides them all (it is modal and
   shows the storage bars in their place -- see app.cpp). */
constexpr int kHeaderTabCount = 4;

/* The tabs are two kinds of thing, and that is what the keys follow now.

   `status`, `details` and `guide` are the pages the player *reads*: L and R move between them in
   the order they are drawn, wrapping at both ends, so a new page of that kind costs no new key
   and the thumbs never leave the shoulder buttons.  The game installer is a *mode* -- + opens it
   deliberately, it is exclusive, B is the only way out -- so it keeps its own key and stays out
   of the ring.  - still opens the guide directly (a shortcut, kept for muscle memory). */
constexpr int kPeerTabCount = 3;

/* One step through the ring.  `current` is the peer index of the page being left, or -1 when the
   page is not one of them (the installer, the agent install flow); those enter at the status
   page, so L / R always move somewhere sensible instead of doing nothing. */
inline int CyclePeerTab(int current, int direction) {
    const int base = current < 0 ? 0 : current % kPeerTabCount;
    const int step = direction < 0 ? -1 : 1;
    return (base + step + kPeerTabCount) % kPeerTabCount;
}

/* Which tabs carry a key badge of their own: the ring's two ends wear Ⓛ / Ⓡ (the shoulder keys
   that move through it), the installer wears its +, and the middle of the ring wears none --
   there is no key that goes *to* it, which is the truth a badge has to tell. */
inline constexpr bool kHeaderTabBadgeAll[kHeaderTabCount] = {true, true, true, true};

struct HeaderTabLayout {
    int badge_x[kHeaderTabCount];  /* left edge of the Ⓐ / Ⓛ / Ⓡ circle */
    int label_x[kHeaderTabCount];  /* left edge of the label text */
    Rect hit[kHeaderTabCount];     /* whole touch target for each entry */
};

/* Lay the entries out from the right edge; index 0 is the left-most one (the guide), the last
   index is the right-most (details).  The caller passes the measured label widths because only
   the font knows them. */
inline HeaderTabLayout LayoutHeaderTabs(int surface_width, const int label_width[kHeaderTabCount],
                                        const bool with_badge[kHeaderTabCount] = kHeaderTabBadgeAll) {
    HeaderTabLayout layout{};
    int x = surface_width - kHeaderMargin;
    for (int i = kHeaderTabCount - 1; i >= 0; --i) {
        x -= label_width[i];
        layout.label_x[i] = x;
        if (with_badge[i]) {
            x -= kHeaderTabBadge + kHeaderTabBadgeGap;
            layout.badge_x[i] = x;
        } else {
            /* No badge: the label keeps its place and the slot collapses, so the strip does not
               show a hole where a key would be. */
            layout.badge_x[i] = layout.label_x[i];
        }
        x -= kHeaderTabSpacing;
    }
    for (int i = 0; i < kHeaderTabCount; ++i) {
        const int left = layout.badge_x[i] - kHeaderTabPadX;
        const int right = layout.label_x[i] + label_width[i] + kHeaderTabPadX;
        layout.hit[i] = Rect{left, kHeaderTabTop - kHeaderTabPadY, right - left,
                             kHeaderTabBadge + 2 * kHeaderTabPadY};
    }
    return layout;
}

}  // namespace acnh_manager::ui
