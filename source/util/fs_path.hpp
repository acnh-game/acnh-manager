#pragma once

#include <switch.h>

#include <cstddef>
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
   buffer belongs to this object and covers the whole declared window, so the window can never
   leave it.

   The platform's own fs client does exactly this.  Checked against the symbol-rich SDK NSO
   (`nn::fs::detail::FileSystemServiceObjectAdapter::DoCreateFile`): it copies the string into a
   zero-filled `FS_MAX_PATH` buffer, returns `fs` `TooLongPath` (6003) when the path does not fit,
   and hands the service *that* buffer with the full declared length -- it never passes the
   caller's pointer and never shrinks the declared size.  Two deliberate differences remain
   here: the buffer is twice `FS_MAX_PATH` (the slack costs nothing and keeps the window well
   inside the object), and an over-long path is truncated rather than reported, which is
   acceptable because every path this app builds is a few tens of bytes long.

   Status of the libnx-side report: it was moved to a repository that is not publicly readable,
   so this repository does not wait for upstream -- the rule is enforced here either way. */
class FsPath {
public:
    /* The invariant the whole class exists for: whatever we hand to fs* has to cover the entire
       declared window by itself.  The assertion is what stops a future edit from shrinking this
       back to something like the string's own length. */
    static constexpr std::size_t kBufferSize = FS_MAX_PATH * 2;
    static_assert(kBufferSize >= FS_MAX_PATH,
                  "an fs* path buffer must cover the whole FS_MAX_PATH IPC window");

    explicit FsPath(const char *path) {
        if (path != nullptr) {
            std::snprintf(m_buf, sizeof(m_buf), "%s", path);
        }
    }
    explicit FsPath(const std::string &path) : FsPath(path.c_str()) {}

    const char *c_str() const { return m_buf; }

private:
    char m_buf[kBufferSize]{};
};

}  // namespace acnh_manager::util
