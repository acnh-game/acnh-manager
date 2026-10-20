#pragma once

/* The transfer's speed over a sliding window, and the time that speed implies.

   A progress screen needs a number a person can act on: "about 8 minutes left".  An average over
   the whole run is the wrong shape for that -- the first seconds (opening the file, creating
   placeholders) are slow and would keep the average pessimistic for minutes, and a run that has
   just stalled would keep showing the optimistic number it had when bytes last moved.  A window of
   a few seconds forgets both: it measures what the link is doing *now*, and a stall makes it fall
   to zero, which the screens render as "no estimate" instead of a false one.

   Pure arithmetic on (time, bytes) points, so the host tests can pin the window's behaviour
   without a console, a network or a clock. */

#include <cstddef>
#include <cstdint>
#include <deque>

namespace acnh_manager::install {

class RateMeter {
public:
    static constexpr std::uint64_t kDefaultWindowMs = 5000;
    /* While bytes move, samples arrive with the writes themselves (one per 4 MB block); while they
       do not, one per second -- that is what lets the window see a stall at all. */
    static constexpr std::uint64_t kIdleSampleMs = 1000;
    /* Shorter than this the figure is noise (a single block's timing), so it is not reported. */
    static constexpr std::uint64_t kMinSpanMs = 500;

    explicit RateMeter(std::uint64_t window_ms = kDefaultWindowMs) : m_window_ms(window_ms) {
        if (m_window_ms == 0) {
            m_window_ms = kDefaultWindowMs;
        }
    }

    /* Starts over from `done` at `now_ms`: the bytes already in place before the transfer began
       (a resumed run, or contents the console already had) are the baseline, not a jump. */
    void Reset(std::uint64_t now_ms, std::uint64_t done) {
        m_points.clear();
        m_points.push_back(Point{now_ms, done});
    }

    /* Feeds one look at the counter.  Safe to call as often as the caller likes: it records a
       point when the byte count moved, or when a second has passed without it moving. */
    void Sample(std::uint64_t now_ms, std::uint64_t done) {
        if (m_points.empty() || now_ms < m_points.back().ms) {
            Reset(now_ms, done); /* first sample, or a clock that went backwards */
            return;
        }
        const Point &back = m_points.back();
        const bool moved = done != back.done;
        if (!moved && now_ms - back.ms < kIdleSampleMs) {
            return;
        }
        m_points.push_back(Point{now_ms, done});
        /* Keep the newest point at or before the window's start, and everything newer: the rate is
           measured from that baseline, so the window is a true "last N seconds" span. */
        const std::uint64_t start = now_ms > m_window_ms ? now_ms - m_window_ms : 0;
        while (m_points.size() > 2 && m_points[1].ms <= start) {
            m_points.pop_front();
        }
    }

    /* Bytes per second over the window; 0 when nothing moved in it (or the span is too short to
       say anything honest). */
    std::uint64_t Rate() const {
        if (m_points.size() < 2) {
            return 0;
        }
        const Point &first = m_points.front();
        const Point &last = m_points.back();
        const std::uint64_t span = last.ms - first.ms;
        if (span < kMinSpanMs || last.done <= first.done) {
            return 0;
        }
        return (last.done - first.done) * 1000 / span;
    }

    /* How many points the window holds; the tests use it to prove a stalled run does not grow the
       queue without bound. */
    std::size_t SampleCount() const { return m_points.size(); }

private:
    struct Point {
        std::uint64_t ms{0};
        std::uint64_t done{0};
    };

    std::uint64_t m_window_ms;
    std::deque<Point> m_points;
};

}  // namespace acnh_manager::install
