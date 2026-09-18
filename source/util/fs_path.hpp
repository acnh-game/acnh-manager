#pragma once

#include <switch.h>

#include <cstdio>
#include <string>

namespace acnh_manager::util {

/* A path handed to fs*.

   libnx declares every path buffer as `FS_MAX_PATH` (0x301) bytes regardless of how long the
   string actually is, and the service maps a **page-aligned window covering that declared
   size**.  A path that lives within `FS_MAX_PATH` of the end of a mapping therefore makes the
   window reach into a page that is not mapped, and the kernel answers `InvalidMemoryState`
   (`0xD401`) -- on a path whose directory is provably fine.

   Measured on hardware (docs/device-acceptance.md): the failing calls all used a pointer whose
   window ran 0x181 bytes past the end of its malloc region (region ended at 0x329C698000, the
   window at 0x329C698181, and the page beyond that was reported unmapped), while every call
   whose window stayed inside its region succeeded in the same session on the same directories.

   Which allocation lands at the end of its region is up to the allocator, so a `std::string`
   built by concatenation can be the last thing in the heap -- and then every install fails
   until the app is restarted.  Copying the path in here first removes that possibility: the
   object is a local, and it is twice `FS_MAX_PATH` long, so the window can never leave it. */
class FsPath {
public:
    explicit FsPath(const char *path) {
        if (path != nullptr) {
            std::snprintf(m_buf, sizeof(m_buf), "%s", path);
        }
    }
    explicit FsPath(const std::string &path) : FsPath(path.c_str()) {}

    const char *c_str() const { return m_buf; }

private:
    char m_buf[FS_MAX_PATH * 2]{};
};

}  // namespace acnh_manager::util
