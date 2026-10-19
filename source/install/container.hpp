#pragma once

/* Container layer: the two container families an install package can use.

       PFS0  NSP / NSZ   flat file table (".nca" entries plus ".tik" / ".cert" / ".xml")
       HFS0  XCI / XCZ   "HEAD" gamecard header at 0x100, root HFS0 at 0xF000, the installable
                         content lives in the "secure" partition

   NSZ / XCZ are the same containers with each .nca payload replaced by an NCZ (zstd) stream;
   the difference shows up in the source layer, so this module only reports `compressed`.

   Pure logic over a random-access reader: no libnx, no file IO, so the host tests drive it
   with an in-memory image.  The byte offsets and the sanity limits follow the reference
   implementation (yati/container/nsp.cpp, xci.cpp), which was checked against real packages.

   `.msp` is refused here on purpose: it is not a Nintendo container but a mod package whose
   romfs/exefs entries land in /atmosphere/contents/<title id>/..., which is the same directory
   the agent installs into (see docs/game_installer_design.md section 7). */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acnh_manager::install {

enum class ContainerKind {
    Unknown,
    Nsp,
    Nsz,
    Xci,
    Xcz,
};

struct ContainerEntry {
    std::string name; /* as stored, lowercased: "e10617820db06889e1638499478da0de.nca" */
    std::uint64_t offset{0};
    std::uint64_t size{0};        /* the NCA this entry stands for (see `compressed`) */
    std::uint64_t packed_size{0}; /* what the entry occupies inside the container */
    /* NSZ / XCZ: this entry is an NCZ stream (a `.ncz` name), so `size` is the NCA it rebuilds --
       which is larger than the bytes on the card.  Reading it means decoding; see install/ncz.hpp. */
    bool compressed{false};
};

struct ContainerInfo {
    ContainerKind kind{ContainerKind::Unknown};
    bool compressed{false}; /* NSZ / XCZ: the .nca payloads are NCZ streams */
    std::uint64_t container_size{0};
    std::vector<ContainerEntry> contents; /* .nca entries, in container order */
    std::vector<ContainerEntry> extra;    /* .tik / .cert / .xml / anything else */
};

/* Random access to the container bytes.  The three sources in the app implement this: a local
   file seeks, the download source issues an HTTP Range request, the upload source asks the
   browser for the next range.  `size == 0` is a valid no-op read. */
class ContainerReader {
public:
    virtual ~ContainerReader() = default;
    virtual bool Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) = 0;
    /* Called by the engine between the steps of a long operation (one content prepared, one block
       decompressed, one piece of a transfer read): whatever else has to keep running -- for the
       upload source, the browser page that feeds it -- gets its turn.  Default: nothing to do.

       This is not decoration.  The console's HTTP server is serviced from this thread, and the read
       loop is the only place that used to do it: while the engine parsed a package, the server was
       deaf, so the page's requests sat in the kernel's accept queue -- `/upload/next` came back 5.4 s
       late and a 512-byte part was accepted 18.9 s late, after which the browser's own 8 s deadlines
       fired and the console declared the browser gone (measured on hardware, 2026-10-17). */
    virtual void Service() {}
};

/* Which file names the SD browser offers and the container layer accepts. */
bool IsSupportedExtension(std::string_view name);

/* True for the mod package the app refuses to install (by extension). */
bool IsModPackageExtension(std::string_view name);

/* Reads the header, the file table and the string table only -- never file data.
   `container_size` is the size of the whole file when the source knows it (0 = unknown);
   every entry is then checked to fit.  On failure `error` carries player-facing wording. */
bool OpenContainer(ContainerReader &reader, std::string_view name, std::uint64_t container_size,
                   ContainerInfo *out, std::string *error);

}  // namespace acnh_manager::install
