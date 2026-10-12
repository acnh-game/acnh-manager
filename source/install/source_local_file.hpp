#pragma once

/* A package sitting on the SD card, as a random-access source.

   The same `ContainerReader` interface the HTTP sources implement, so the parser, the task and
   the installer do not care where the bytes came from.  This is also the only source that can be
   read without any network at all, which is why it lands first and makes the engine testable on
   the console before the HTTP work. */

#include <switch.h>

#include <cstdint>
#include <string>

#include "install/container.hpp"

namespace acnh_manager::install {

class LocalFileSource final : public ContainerReader {
public:
    LocalFileSource() = default;
    ~LocalFileSource() override;
    LocalFileSource(const LocalFileSource &) = delete;
    LocalFileSource &operator=(const LocalFileSource &) = delete;

    /* Opens the file and reports its size; `size` is what the task file records so a resume can
       tell "the same package" from "some other file with the same name". */
    bool Open(FsFileSystem *fs, const std::string &path, std::uint64_t *size, std::string *error);
    void Close();
    bool IsOpen() const { return m_open; }
    const std::string &Path() const { return m_path; }

    bool Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) override;

private:
    FsFile m_file{};
    std::string m_path;
    std::uint64_t m_size{0};
    bool m_open{false};
};

}  // namespace acnh_manager::install
