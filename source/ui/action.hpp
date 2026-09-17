#pragma once

/* Actions and the focus model shared by touch and buttons.

   Every interactive control is one Action: an id, the rectangle it occupies and whether it is
   usable right now.  Touch hit-tests it, the d-pad/stick moves focus between actions and A
   activates the focused one -- all three go through this header.  It is pure C++ (no libnx)
   because the layout and interaction bugs we hit earlier were only visible on the console;
   the host tests below pin the behaviour instead. */

#include <cstddef>
#include <vector>

namespace acnh_manager::ui {

struct Rect {
    int x{0};
    int y{0};
    int w{0};
    int h{0};
};

inline constexpr bool Contains(const Rect &r, int px, int py) {
    return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}

struct Action {
    int id{-1};
    Rect rect{};
    bool enabled{true};
};

/* Id of the enabled action containing (x, y), or -1.  Disabled actions never hit. */
inline int HitTest(const std::vector<Action> &actions, int x, int y) {
    for (const Action &action : actions) {
        if (action.enabled && Contains(action.rect, x, y)) {
            return action.id;
        }
    }
    return -1;
}

/* First enabled action, or -1 (used as the entry focus). */
inline int FirstEnabled(const std::vector<Action> &actions) {
    for (const Action &action : actions) {
        if (action.enabled) {
            return action.id;
        }
    }
    return -1;
}

/* Spatial focus movement: from `current` (or -1) move in (dx, dy) and return the nearest
   enabled action in that direction, keeping `current` when there is none.

   Focus stays inside the current row/column: a candidate has to overlap the current control
   on the perpendicular axis (so pressing right never jumps to a button that sits above or
   below), and of those the closest one along the move wins, with the off-axis distance as a
   tie-break.  A disabled neighbour simply means "no move" -- which is what the user expects
   from a greyed-out button. */
inline int MoveFocus(const std::vector<Action> &actions, int current, int dx, int dy) {
    const Action *from = nullptr;
    for (const Action &action : actions) {
        if (action.id == current) {
            from = &action;
        }
    }
    if (from == nullptr) {
        return FirstEnabled(actions);
    }
    const int cx = from->rect.x + from->rect.w / 2;
    const int cy = from->rect.y + from->rect.h / 2;
    int best = current;
    int best_score = 0;
    for (const Action &action : actions) {
        if (action.id == current || !action.enabled) {
            continue;
        }
        const int ax = action.rect.x + action.rect.w / 2;
        const int ay = action.rect.y + action.rect.h / 2;
        const int delta_x = ax - cx;
        const int delta_y = ay - cy;
        const int primary = dx != 0 ? delta_x * dx : delta_y * dy;
        if (primary <= 0) {
            continue; /* not in the direction we are moving */
        }
        /* Perpendicular overlap: moving right/down must stay in the same row/column. */
        const bool horizontal = dx != 0;
        const int current_lo = horizontal ? from->rect.y : from->rect.x;
        const int current_hi =
            horizontal ? from->rect.y + from->rect.h : from->rect.x + from->rect.w;
        const int candidate_lo = horizontal ? action.rect.y : action.rect.x;
        const int candidate_hi =
            horizontal ? action.rect.y + action.rect.h : action.rect.x + action.rect.w;
        if (candidate_hi <= current_lo || candidate_lo >= current_hi) {
            continue;
        }
        const int secondary = horizontal ? (delta_y < 0 ? -delta_y : delta_y)
                                         : (delta_x < 0 ? -delta_x : delta_x);
        const int score = primary + secondary * 3;
        if (best == current || score < best_score) {
            best = action.id;
            best_score = score;
        }
    }
    return best;
}

}  // namespace acnh_manager::ui
