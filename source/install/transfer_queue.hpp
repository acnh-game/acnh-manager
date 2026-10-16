#pragma once

/* The hand-off between the thread that *reads* a package and the thread that *writes* it.

   The reference implementation runs the transfer as three threads around a ring of buffers (read
   / decompress / write, `yati.cpp`).  Two are enough here, and which half does what is a measured
   decision (2026-10-16): the **fetch thread only reads the source** -- it does not decode -- and
   the writer (the UI thread, so the placeholders keep being written in the one order the task
   expects and the screen keeps drawing) runs the NCZ decoder, re-encrypts, and writes.  Decoding on
   the fetch thread cost a third of the transfer (7.3 vs 10.0 MB/s): the zstd work and the network
   reads were competing for the same cores and memory bandwidth.  This queue is the only state the
   two threads share.

   Slots are fixed at construction: a ring of a few block-sized buffers, never reallocated mid
   transfer.  They stay in order -- the fetch thread publishes pieces in the order the contents run,
   and the writer takes them in the same order -- because the placeholder offsets depend on it.  A
   piece marked `payload` is still compressed: the writer feeds it to the decoder before writing. */

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "install/install_task.hpp"

namespace acnh_manager::install {

/* One prepared range: what the task asked for, where it starts inside its content, and how many
   bytes are in the slot. */
struct PreparedRange {
    RangeRequest range;
    std::uint64_t content_offset{0};
    std::size_t size{0};
    /* True when the slot holds *payload* (the compressed bytes of an NCZ entry) rather than content
       bytes: the writer decodes it before writing.  The decode used to run on the reader thread,
       which put its 3.4 s of zstd work on the same critical path as 46 s of network reads
       (measured 2026-10-16); the writer has the slack to carry it instead. */
    bool payload{false};
};

class TransferQueue {
public:
    /* `slots` buffers of `slot_size` bytes each.  Three 4 MB slots (12 MB) is what the reference's
       ring holds, and enough for the reader to stay a range ahead of the writer. */
    TransferQueue(std::size_t slots, std::size_t slot_size);
    ~TransferQueue();
    TransferQueue(const TransferQueue &) = delete;
    TransferQueue &operator=(const TransferQueue &) = delete;

    /* ---- producer (the reader thread) ---- */

    /* Waits for a free slot and hands back its buffer; nullptr when the consumer asked everyone to
       stop, or the queue already failed.  Fill it, then Publish. */
    std::uint8_t *AcquireSlot();
    void Publish(const PreparedRange &range);
    /* Records a failure the consumer will surface, and wakes it. */
    void Fail(std::string message);
    /* No more ranges will be produced. */
    void Finish();

    /* ---- consumer (the UI thread) ---- */

    enum class Take {
        Got,      /* `*range` and `*data` are valid until Release() */
        Empty,    /* nothing ready before the deadline: the caller draws and asks again */
        Finished, /* the producer is done and nothing is left */
        Failed,   /* the producer failed; `*error` says why */
        Stopped,  /* someone asked to stop */
    };
    Take Next(PreparedRange *range, std::uint8_t **data, std::string *error, int timeout_ms);
    /* Gives the slot from the last Got back to the producer. */
    void Release();

    /* Either side: everybody out (the app is closing, or an error ended the run). */
    void RequestStop();
    bool StopRequested();

private:
    struct Entry {
        std::size_t slot{0};
        PreparedRange range;
    };

    /* Not const: the predicate helpers take the lock. */
    mutable std::mutex m_mutex;
    std::condition_variable m_ready; /* work available */
    std::condition_variable m_free;  /* a slot came back */
    std::vector<std::vector<std::uint8_t>> m_slots;
    std::deque<Entry> m_items;
    std::vector<std::size_t> m_free_slots;
    std::size_t m_producing{0};
    bool m_producing_valid{false};
    std::size_t m_consuming{0};
    bool m_consuming_valid{false};
    bool m_finished{false};
    bool m_stopped{false};
    bool m_failed{false};
    std::string m_error;
};

}  // namespace acnh_manager::install
