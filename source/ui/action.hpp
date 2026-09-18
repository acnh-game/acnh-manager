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
    /* Controls that already have a dedicated key (A/X/Y/L/R/B) are not focus targets: the key
       badge already says everything about them, so a focus ring on them is noise.  Only
       controls without their own key can be focused (today: the details page's language row). */
    bool focusable{false};
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

inline int AbsInt(int value) { return value < 0 ? -value : value; }

/* Result of feeding one touch poll into TapTracker. */
struct TapResult {
    bool press_edge{false};   /* a new contact started during this poll */
    int pressed_action{-1};   /* action under the finger then (-1 when the finger missed) */
    int tapped_action{-1};    /* action to fire: press and release landed on the same one */
};

/* Tap tracking: a tap fires only when the finger goes down and comes back up on the same
   enabled action, having stayed within the slop of where it landed the whole time (a swipe
   that wanders off and returns is a drag, not a tap).

   The panel only reports coordinates *while* a finger is down -- the poll that sees the
   release carries no position -- so the tracker remembers where the finger last was and uses
   that for both the drag test and the release hit test.  Reading the release coordinates from
   that poll directly is what made every real tap look like a screen-wide drag on hardware. */
class TapTracker {
public:
    TapResult Update(const std::vector<Action> &actions, bool down, int x, int y) {
        TapResult result{};
        if (down) {
            if (!m_down) {
                m_down = true;
                m_pressed = HitTest(actions, x, y);
                m_press_x = x;
                m_press_y = y;
                m_max_deviation = 0;
                result.press_edge = true;
                result.pressed_action = m_pressed;
            }
            m_last_x = x;
            m_last_y = y;
            const int deviation = AbsInt(m_last_x - m_press_x) > AbsInt(m_last_y - m_press_y)
                                      ? AbsInt(m_last_x - m_press_x)
                                      : AbsInt(m_last_y - m_press_y);
            if (deviation > m_max_deviation) {
                m_max_deviation = deviation;
            }
            return result;
        }
        if (!m_down) {
            return result;
        }
        m_down = false;
        const int pressed = m_pressed;
        m_pressed = -1;
        if (pressed < 0) {
            return result;
        }
        if (m_max_deviation > kSlop) {
            return result;
        }
        if (HitTest(actions, m_last_x, m_last_y) != pressed) {
            return result;
        }
        result.tapped_action = pressed;
        return result;
    }

    bool Down() const { return m_down; }

private:
    /* Half a fingertip: the contact patch measured on hardware is about 67x89 px, so a
       deliberate press can wobble a little without turning into a drag. */
    static constexpr int kSlop = 32;
    bool m_down{false};
    int m_pressed{-1};
    int m_press_x{0};
    int m_press_y{0};
    int m_last_x{0};
    int m_last_y{0};
    int m_max_deviation{0};
};

/* First enabled action, or -1 (used as the entry focus). */
inline int FirstEnabled(const std::vector<Action> &actions) {
    for (const Action &action : actions) {
        if (action.enabled && action.focusable) {
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
        if (action.id == current || !action.enabled || !action.focusable) {
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
