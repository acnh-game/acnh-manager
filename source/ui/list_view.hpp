#pragma once

/* The scrolling, focusable list the installer's file picker needs -- the app's first one.

   Pure geometry and index arithmetic (no libnx, no drawing) for the same reason the header tabs
   are: what is drawn and what a tap hits have to come from one place, and "which row has the
   focus after three downs and a scroll" is cheap to pin down with tests and expensive to debug
   on a console.  A row that cannot be selected (an unsupported file) is skipped by the focus,
   never focused-and-ignored. */

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace acnh_manager::ui {

struct ListItem {
    std::string name;      /* the file or directory name */
    std::string note;      /* right-aligned hint: "directory", "package", a size */
    bool selectable{true}; /* false = drawn dimmed, never focused */
};

/* Which items the visible window shows. */
struct ListWindow {
    int first{0};
    int count{0};
};

class ListView {
public:
    static constexpr int kRowHeight = 66; /* one row plus its gap, as in the sketch */

    void SetItems(std::vector<ListItem> items) {
        m_items = std::move(items);
        m_focus = FirstSelectable();
        m_first = 0;
        Clamp();
    }

    void SetBox(int width, int height) {
        m_width = width;
        m_height = height;
        Clamp();
    }

    const std::vector<ListItem> &Items() const { return m_items; }
    int Focus() const { return m_focus; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

    int VisibleRows() const {
        if (m_height <= 0) {
            return 0;
        }
        const int rows = m_height / kRowHeight;
        return rows > 0 ? rows : 1;
    }

    /* Moves the focus by `step` selectable rows, stopping at the ends rather than wrapping: a
       picker that jumps from the last file to the first one loses the player's place. */
    void MoveFocus(int step) {
        if (m_items.empty() || step == 0) {
            return;
        }
        int index = m_focus >= 0 ? m_focus : FirstSelectable();
        const int direction = step > 0 ? 1 : -1;
        const int count = step > 0 ? step : -step;
        for (int i = 0; i < count; ++i) {
            const int next = NextSelectable(index, direction);
            if (next < 0) {
                break;
            }
            index = next;
        }
        SetFocus(index);
    }

    void SetFocus(int index) {
        if (index < 0 || index >= static_cast<int>(m_items.size())) {
            return;
        }
        if (!m_items[static_cast<std::size_t>(index)].selectable) {
            return;
        }
        m_focus = index;
        Clamp();
    }

    ListWindow Window() const {
        ListWindow window;
        if (m_items.empty() || m_height <= 0) {
            return window;
        }
        window.count = VisibleRows();
        window.first = m_first;
        const int size = static_cast<int>(m_items.size());
        if (window.first + window.count > size) {
            window.first = size - window.count;
            if (window.first < 0) {
                window.first = 0;
            }
        }
        return window;
    }

    /* The item under a y offset inside the list box, or -1 when the tap hit nothing. */
    int RowAt(int y) const {
        if (y < 0 || m_items.empty()) {
            return -1;
        }
        const int row = y / kRowHeight;
        if (row >= VisibleRows()) {
            return -1;
        }
        const int index = m_first + row;
        return index < static_cast<int>(m_items.size()) ? index : -1;
    }

    int FirstVisible() const { return m_first; }

private:
    int FirstSelectable() const {
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i].selectable) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /* The next selectable index in `direction`, or -1 when there is none before the end. */
    int NextSelectable(int from, int direction) const {
        int index = from;
        for (;;) {
            index += direction;
            if (index < 0 || index >= static_cast<int>(m_items.size())) {
                return -1;
            }
            if (m_items[static_cast<std::size_t>(index)].selectable) {
                return index;
            }
        }
    }

    void Clamp() {
        if (m_focus < 0) {
            m_first = 0;
            return;
        }
        const int rows = VisibleRows();
        if (rows <= 0) {
            m_first = 0;
            return;
        }
        if (m_focus < m_first) {
            m_first = m_focus;
        }
        if (m_focus >= m_first + rows) {
            m_first = m_focus - rows + 1;
        }
        if (m_first < 0) {
            m_first = 0;
        }
    }

    std::vector<ListItem> m_items;
    int m_focus{-1};
    int m_first{0};
    int m_width{0};
    int m_height{0};
};

}  // namespace acnh_manager::ui
