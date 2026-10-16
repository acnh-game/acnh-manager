#include "install/transfer_queue.hpp"

#include <chrono>
#include <utility>

namespace acnh_manager::install {

TransferQueue::TransferQueue(std::size_t slots, std::size_t slot_size) {
    if (slots == 0) {
        slots = 1;
    }
    m_slots.resize(slots, std::vector<std::uint8_t>(slot_size));
    for (std::size_t i = 0; i < slots; ++i) {
        m_free_slots.push_back(i);
    }
}

TransferQueue::~TransferQueue() = default;

std::uint8_t *TransferQueue::AcquireSlot() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_free.wait(lock, [this] { return !m_free_slots.empty() || m_stopped || m_failed; });
    if (m_stopped || m_failed) {
        return nullptr;
    }
    const std::size_t slot = m_free_slots.back();
    m_free_slots.pop_back();
    m_producing = slot;
    m_producing_valid = true;
    return m_slots[slot].data();
}

void TransferQueue::Publish(const PreparedRange &range) {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_producing_valid) {
            /* The consumer asked to stop while this slot was being filled: drop it. */
            return;
        }
        Entry entry;
        entry.slot = m_producing;
        entry.range = range;
        m_items.push_back(std::move(entry));
        m_producing_valid = false;
    }
    m_ready.notify_all();
}

void TransferQueue::Fail(std::string message) {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_failed = true;
        m_error = std::move(message);
    }
    m_ready.notify_all();
    m_free.notify_all();
}

void TransferQueue::Finish() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_finished = true;
    }
    m_ready.notify_all();
}

TransferQueue::Take TransferQueue::Next(PreparedRange *range, std::uint8_t **data,
                                        std::string *error, int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_items.empty() && !m_stopped && !m_failed && !m_finished) {
        m_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms),
                         [this] {
                             return !m_items.empty() || m_stopped || m_failed || m_finished;
                         });
    }
    if (m_stopped) {
        return Take::Stopped;
    }
    if (!m_items.empty()) {
        Entry entry = m_items.front();
        m_items.pop_front();
        *range = entry.range;
        *data = m_slots[entry.slot].data();
        m_consuming = entry.slot;
        m_consuming_valid = true;
        return Take::Got;
    }
    if (m_failed) {
        if (error != nullptr) {
            *error = m_error;
        }
        return Take::Failed;
    }
    if (m_finished) {
        return Take::Finished;
    }
    return Take::Empty;
}

void TransferQueue::Release() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_consuming_valid) {
            return;
        }
        m_free_slots.push_back(m_consuming);
        m_consuming_valid = false;
    }
    m_free.notify_all();
}

void TransferQueue::RequestStop() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stopped = true;
    }
    m_ready.notify_all();
    m_free.notify_all();
}

bool TransferQueue::StopRequested() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_stopped;
}

}  // namespace acnh_manager::install
