#include "install/source_memory.hpp"

#include <cstring>

namespace acnh_manager::install {

bool MemorySource::Open(const std::uint8_t *data, std::size_t size, std::string *error) {
    Close();
    if (data == nullptr || size == 0) {
        if (error != nullptr) {
            *error = "the embedded package is empty";
        }
        return false;
    }
    m_data = data;
    m_size = size;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void MemorySource::Close() {
    m_data = nullptr;
    m_size = 0;
}

bool MemorySource::Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) {
    if (m_data == nullptr || out == nullptr || offset > m_size || size > m_size - offset) {
        return false;
    }
    if (size != 0) {
        std::memcpy(out, m_data + offset, size);
    }
    return true;
}

}  // namespace acnh_manager::install
