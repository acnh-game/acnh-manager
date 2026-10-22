#pragma once

/* A container that is already in this process's memory.  It is used for packages carried inside
   the NRO itself, so the install engine can read them through exactly the same container path as
   an SD file, a download or a browser upload.  The bytes must outlive the source and the engine. */

#include <cstddef>
#include <cstdint>
#include <string>

#include "install/container.hpp"

namespace acnh_manager::install {

class MemorySource final : public ContainerReader {
public:
    bool Open(const std::uint8_t *data, std::size_t size, std::string *error);
    void Close();
    bool IsOpen() const { return m_data != nullptr; }

    bool Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) override;

private:
    const std::uint8_t *m_data{nullptr};
    std::uint64_t m_size{0};
};

}  // namespace acnh_manager::install
