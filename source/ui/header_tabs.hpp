#pragma once

/* Header tab geometry, in the order it is drawn: "+ game install", "Ⓛ status", "details",
   "guide Ⓡ" -- the installer first, then the ring of pages L / R move between, with the two
   shoulder keys bracketing that group.

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

/* Where an entry's key badge sits relative to its label.  The installer's + and the ring's
   opening Ⓛ go *before* their label; the ring's closing Ⓡ goes *after* the last page, so the two
   shoulder keys read as the brackets of the group they move through:

       + 游戏安装/更新    Ⓛ 状态  详情  功能说明 Ⓡ

   The middle of the ring carries none: no key goes straight to it, and a badge would promise one
   that does not exist. */
enum class TabBadge {
    None,
    Before,
    After,
};

/* What a caller that says nothing about badges gets: one in front of every label. */
inline constexpr TabBadge kHeaderTabBadgeBefore[kHeaderTabCount] = {
    TabBadge::Before, TabBadge::Before, TabBadge::Before, TabBadge::Before};

struct HeaderTabLayout {
    int badge_x[kHeaderTabCount];  /* left edge of the Ⓐ / Ⓛ / Ⓡ circle */
    int label_x[kHeaderTabCount];  /* left edge of the label text */
    Rect hit[kHeaderTabCount];     /* whole touch target for each entry */
};

/* Lay the entries out from the right edge; index 0 is the left-most one (the guide), the last
   index is the right-most (details).  The caller passes the measured label widths because only
   the font knows them. */
inline HeaderTabLayout LayoutHeaderTabs(int surface_width, const int label_width[kHeaderTabCount],
                                        const TabBadge badge[kHeaderTabCount] =
                                            kHeaderTabBadgeBefore) {
    HeaderTabLayout layout{};
    int x = surface_width - kHeaderMargin;
    for (int i = kHeaderTabCount - 1; i >= 0; --i) {
        /* Consume this entry from the right edge inwards.  What it spans is exactly what it
           draws, and that span is its touch target -- one number, no "remember the badge width
           here and the label width there" (which is how a closing badge ends up outside its own
           target). */
        const int entry_right = x;
        if (badge[i] == TabBadge::After) {
            x -= kHeaderTabBadge;
            layout.badge_x[i] = x;
            x -= kHeaderTabBadgeGap;
        }
        x -= label_width[i];
        layout.label_x[i] = x;
        if (badge[i] == TabBadge::Before) {
            x -= kHeaderTabBadge + kHeaderTabBadgeGap;
            layout.badge_x[i] = x;
        } else if (badge[i] == TabBadge::None) {
            /* No badge: nothing is reserved, so the strip does not show a hole where a key would
               be.  (Its badge_x is the label's x, which keeps the drawing code's one expression
               for "where the badge would be" honest.) */
            layout.badge_x[i] = layout.label_x[i];
        }
        const int entry_left = x;
        layout.hit[i] = Rect{entry_left - kHeaderTabPadX, kHeaderTabTop - kHeaderTabPadY,
                             (entry_right - entry_left) + 2 * kHeaderTabPadX,
                             kHeaderTabBadge + 2 * kHeaderTabPadY};
        x -= kHeaderTabSpacing;
    }
    return layout;
}

}  // namespace acnh_manager::ui
