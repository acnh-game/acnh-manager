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

/* Fills `task->contents` from the CNMT: one entry per content, in CNMT order, with the byte range
   it occupies inside the container.  A content the package does not actually carry is an error
   here rather than a surprise while fetching. */
bool FillTaskContents(const PackageInfo &package, InstallTask *task, std::string *error);

/* The two skips the design promises, decided from numbers alone (design section 9, step 4). */
enum class SkipDecision {
    Install,
    AlreadyInstalled, /* same version, and the console already has every content */
    NewerInstalled,   /* the console's version is higher and downgrades are off */
};
SkipDecision DecideSkip(const InstallOptions &options, std::uint32_t package_version,
                        std::uint32_t installed_version, bool same_version_installed);

/* The label the result page shows for a package (base / update / dlc), from the CNMT type. */
const char *MetaTypeLabel(MetaType type);

/* The application (base game) id a package's title id belongs to: a patch lives at
   base + 0x800, an add-on at base + 0x1000 + index, and everything the console offers about the
   *game* -- its control data, its icon, its name -- is filed under the base id.  The offset sits
   in one nibble, so it is masked out rather than compared against the whole low word (the
   reference implementation's GetAppId). */
std::uint64_t ApplicationIdOf(std::uint64_t title_id, MetaType type);

}  // namespace acnh_manager::install
