#pragma once

/* "What is this package?" -- the one place that combines the container, the NCA header and the
   CNMT, and the rules that turn that into a decision.

   Everything here is either pure or a thin read through the `ContainerReader`, so the host tests
   drive it with in-memory containers (see tests/host_tests.cpp). The console-specific parts --
   the header key and the ticket import -- sit in nca_crypto.* and nx_services.*. */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "install/container.hpp"
#include "install/install_task.hpp"
#include "install/nca.hpp"
#include "install/options.hpp"

namespace acnh_manager::install {

struct PackageInfo {
    ContainerInfo container;
    std::string meta_content_name; /* the .nca whose payload is the CNMT */
    CnmtInfo cnmt;
    std::vector<std::uint8_t> meta_blob; /* the CNMT payload, stored verbatim in the meta database */
    std::vector<std::uint8_t> ticket;    /* empty when the package carries none (a base game) */
    std::vector<std::uint8_t> certificate;
};

/* Reads the directory, the meta NCA's header (decrypted through spl) and its CNMT payload, plus
   the ticket pair.  After this the caller knows what the package is; nothing is written yet. */
bool ReadPackage(ContainerReader &reader, std::string_view name, std::uint64_t container_size,
                 PackageInfo *out, std::string *error);

/* Fills `task->contents` with the union of every meta's contents: one entry per content, in meta
   order, with the byte range it occupies inside the container.  A container can carry several
   metas (a cartridge dump made from a game plus its update and DLCs has one per piece, and the
   reference implementation installs all of them in one run), and a content two of them share --
   an update reusing the base game's data -- appears once.  A content a meta lists but the package
   does not carry is an error here rather than a surprise while fetching. */
bool FillTaskContents(const std::vector<CnmtInfo> &metas, const ContainerInfo &container,
                      InstallTask *task, std::string *error);

/* The bytes the console stores as this package's content-meta record (and reads back when it wants
   a title's contents, its control data, its icon and its version).

   This is *not* the CNMT file, although it holds the same header, extended header and content
   entries: the console's record starts with the 8-byte ContentMetaHeader (the CNMT file keeps that
   at offset 0x0E behind the title id), then the extended header, then one 24-byte content info per
   content -- the 32-byte SHA-256 that the CNMT carries in front of each entry is not part of it --
   and finally the content-meta infos.  The package's own meta content (the .cnmt.nca) is listed
   first and is not in the CNMT's own table, so it is added here; without it the console cannot
   find a title's control data at all (measured: the home menu drew an empty tile and
   `nsGetApplicationControlData` answered 0x8C10 while the record itself existed). */
bool BuildMetaRecordBlob(const std::vector<std::uint8_t> &cnmt_payload,
                         const std::string &meta_content_name, const ContainerInfo &container,
                         std::vector<std::uint8_t> *out, std::string *error);

/* The two skips the design promises, decided from numbers alone (design section 9, step 4).
   `has_this_version` means "this exact version is installed *and* the console's application record
   points at it" -- a version that only exists in the meta database is not something the console
   can use, and the caller passes false so the install repairs the record instead of reporting
   "nothing to do". */
enum class SkipDecision {
    Install,
    AlreadyInstalled, /* same version, and the console already has it registered */
    NewerInstalled,   /* the console's version is higher and downgrades are off */
};
SkipDecision DecideSkip(const InstallOptions &options, std::uint32_t package_version,
                        std::uint32_t installed_version, bool has_this_version);

/* The label the result page shows for a package (base / update / dlc), from the CNMT type. */
const char *MetaTypeLabel(MetaType type);

/* The application (base game) id a package's title id belongs to: a patch lives at
   base + 0x800, an add-on at base + 0x1000 + index, and everything the console offers about the
   *game* -- its control data, its icon, its name -- is filed under the base id.  The offset sits
   in one nibble, so it is masked out rather than compared against the whole low word (the
   reference implementation's GetAppId). */
std::uint64_t ApplicationIdOf(std::uint64_t title_id, MetaType type);

}  // namespace acnh_manager::install
