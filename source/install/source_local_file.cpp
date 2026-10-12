#include "install/source_local_file.hpp"

#include <cstdio>

namespace acnh_manager::install {
namespace {

std::string Describe(const char *what, Result rc) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08x)", what, static_cast<unsigned>(rc));
    return buffer;
}

}  // namespace

LocalFileSource::~LocalFileSource() { Close(); }

bool LocalFileSource::Open(FsFileSystem *fs, const std::string &path, std::uint64_t *size,
                           std::string *error) {
    Close();
    if (fs == nullptr || path.empty()) {
        *error = "no file was chosen";
        return false;
    }
    const Result rc = fsFsOpenFile(fs, path.c_str(), FsOpenMode_Read, &m_file);
    if (R_FAILED(rc)) {
        *error = Describe("opening the package", rc);
        return false;
    }
    s64 file_size = 0;
    const Result stat = fsFileGetSize(&m_file, &file_size);
    if (R_FAILED(stat) || file_size < 0) {
        fsFileClose(&m_file);
        *error = Describe("reading the package size", R_FAILED(stat) ? stat : MAKERESULT(1, 1));
        return false;
    }
    m_path = path;
    m_size = static_cast<std::uint64_t>(file_size);
    m_open = true;
    *size = m_size;
    return true;
}

void LocalFileSource::Close() {
    if (m_open) {
        fsFileClose(&m_file);
        m_open = false;
    }
    m_path.clear();
    m_size = 0;
}

bool LocalFileSource::Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) {
    if (!m_open) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (offset > m_size || size > m_size - offset) {
        return false; /* a short file is a truncated download, never a valid read */
    }
    /* fsFileRead takes a 64-bit offset and returns what it actually read: a partial read here
       would silently hand the parser zeros, so it is treated as a failure. */
    u64 read = 0;
    const Result rc = fsFileRead(&m_file, offset, out, size, FsReadOption_None, &read);
    return R_SUCCEEDED(rc) && read == size;
}

}  // namespace acnh_manager::install
