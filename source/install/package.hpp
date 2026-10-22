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

/* A ticket and the certificate that goes with it, named by the rights id they belong to: a
   package stores them as `<rights id>.tik` and `<rights id>.cert`.  A content whose NCA carries a
   rights id is encrypted with that title key, so the console can only read it once this pair has
   been imported -- which is why a real update downloaded from Nintendo carries one (measured:
   `[UPD][v3.0.3][01006F8002326800]` has `<rights id>.tik` + `.cert` next to its four contents). */
struct PackageTicket {
    std::string rights_id; /* 32 hex digits, as the file names spell it */
    std::vector<std::uint8_t> ticket;
    std::vector<std::uint8_t> certificate;
};

struct PackageInfo {
    ContainerInfo container;
    std::string meta_content_name; /* the .nca whose payload is the CNMT */
    CnmtInfo cnmt;
    std::vector<std::uint8_t> meta_blob; /* the CNMT payload, stored verbatim in the meta database */
    /* Every ticket the container carries, in the order the file table lists them.  Empty for a
       package that needs none, which is the normal case for a cartridge dump. */
    std::vector<PackageTicket> tickets;
};

/* Reads the container's directory and the ticket pairs, and points at the meta content by name
   (`meta_content_name`; a container that stores it compressed names it `.cnmt.ncz`).  The CNMT
   itself is *not* read here: it lives inside that content, which only the system's own file layer
   can open, so the device side installs that one content and mounts it (GameInstall::Prepare).
   After this the caller knows what the container holds; nothing is written yet. */
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
/* `lower_required_system_version` zeroes the firmware version an application or a patch asks for
   inside that record, which is what the reference implementation does by default
   (`lower_system_version`): a package built for newer firmware installs otherwise, and the console
   then refuses to launch the title with "a system update is required".  Nothing the console
   verifies covers this field -- it is read from the record, not from anything signed -- so zeroing
   it leaves the record consistent, and it is the reference's long-standing default. */
/* A packaged HOME-menu forwarder uses the reference frontend installer's record shape: the Meta
   content-info size is the CNMT payload size and the record header storage is zero.  Keep that
   distinction explicit instead of passing magic override values from the installer. */
enum class MetaRecordShape {
    Console,
    Forwarder,
};

bool BuildMetaRecordBlob(const std::vector<std::uint8_t> &cnmt_payload,
                         const std::string &meta_content_name, const ContainerInfo &container,
                         bool lower_required_system_version, MetaRecordShape shape,
                         std::vector<std::uint8_t> *out, std::string *error);

/* The two skips the design promises, decided from numbers alone (design section 9, step 4).
   `has_this_version` means "this exact version is installed *and* the console's application record
   points at it" -- a version that only exists in the meta database is not something the console
   can use, and the caller passes false so the install repairs the record instead of reporting
   "nothing to do". */
enum class SkipDecision {
    Install,
    AlreadyInstalled, /* same version, and the console already has it registered */
    NewerInstalled,   /* an update older than the one the console has, and downgrades are off */
};
/* The decision for one package.  Only an *update* is held back by a newer version being installed:
   the reference implementation compares versions for patches alone (`GetLatestVersion`), and a game
   or an add-on has to install anyway -- a base game whose contents were deleted while its update's
   record stayed behind would otherwise be "already covered by a newer version" and could never be
   repaired.  `has_this_version` is what stops a second run of the same package. */
SkipDecision DecideSkip(const InstallOptions &options, MetaType type, std::uint32_t package_version,
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
