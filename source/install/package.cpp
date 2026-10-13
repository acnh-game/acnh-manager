#include "install/package.hpp"

#include <algorithm>
#include <cstring>


namespace acnh_manager::install {
namespace {

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* The .nca file of a content, by the name the container gives it. */
const ContainerEntry *FindContentEntry(const ContainerInfo &container, const std::string &id) {
    for (const auto &entry : container.contents) {
        if (entry.name.compare(0, id.size(), id) == 0 && EndsWith(entry.name, ".nca") &&
            entry.name.find('.') == id.size()) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

bool ReadPackage(ContainerReader &reader, std::string_view name, std::uint64_t container_size,
                 PackageInfo *out, std::string *error) {
    *out = PackageInfo{};
    if (!OpenContainer(reader, name, container_size, &out->container, error)) {
        return false;
    }

    /* The meta NCA is the one whose name says so; that is how every package in circulation names
       it, and it saves decrypting headers until the right one is found. */
    const ContainerEntry *meta = nullptr;
    for (const auto &entry : out->container.contents) {
        if (EndsWith(entry.name, ".cnmt.nca")) {
            meta = &entry;
            break;
        }
    }
    if (meta == nullptr) {
        *error = "this package has no meta content (nothing to read the CNMT from)";
        return false;
    }
    out->meta_content_name = meta->name;

    /* The CNMT is *not* read here: it lives inside the meta content, whose encryption only the
       system's own file layer understands.  The device side installs that one content and mounts
       it (see GameInstall::Prepare and the reference implementation's ParseCnmt); this function
       stays pure so the host tests keep covering the container itself. */

    /* Tickets are optional: a base game shipped without one is still installable (the console
       already has the ticket when it matters). */
    for (const auto &entry : out->container.extra) {
        if (EndsWith(entry.name, ".tik") && out->ticket.empty()) {
            out->ticket.resize(static_cast<std::size_t>(entry.size));
            if (!reader.Read(entry.offset, out->ticket.size(), out->ticket.data())) {
                *error = "cannot read the ticket";
                return false;
            }
        } else if (EndsWith(entry.name, ".cert") && out->certificate.empty()) {
            out->certificate.resize(static_cast<std::size_t>(entry.size));
            if (!reader.Read(entry.offset, out->certificate.size(), out->certificate.data())) {
                *error = "cannot read the certificate";
                return false;
            }
        }
    }
    return true;
}

bool FillTaskContents(const PackageInfo &package, InstallTask *task, std::string *error) {
    task->contents.clear();
    task->contents.reserve(package.cnmt.contents.size());
    for (const auto &content : package.cnmt.contents) {
        if (content.type == ContentType::DeltaFragment) {
            /* Delta fragments are listed by the CNMT but not shipped inside the package: they are
               the "what changed since the version you might have" data, and the console does not
               need them from us.  The reference implementation skips them for the same reason --
               without this, every update package would look like it is missing a content. */
            continue;
        }
        const ContainerEntry *entry = FindContentEntry(package.container, content.content_id);
        if (entry == nullptr) {
            *error = "the package lists a content it does not carry: " + content.content_id;
            return false;
        }
        TaskContent item;
        item.name = entry->name;
        item.offset = entry->offset;
        item.size = entry->size;
        item.content_id = content.content_id;
        switch (content.type) {
            case ContentType::Meta: item.kind = ContentKind::Meta; break;
            case ContentType::Program: item.kind = ContentKind::Program; break;
            case ContentType::Control: item.kind = ContentKind::Control; break;
            case ContentType::Manual: item.kind = ContentKind::Legal; break;
            case ContentType::Data: item.kind = ContentKind::Data; break;
            case ContentType::PublicData: item.kind = ContentKind::PublicData; break;
            case ContentType::DeltaFragment: item.kind = ContentKind::DeltaFragment; break;
            case ContentType::Unknown: item.kind = ContentKind::Unknown; break;
        }
        task->contents.push_back(std::move(item));
    }
    /* The meta content first: it carries the CNMT, so reading it early is what lets the console
       answer "is this already installed?" before the multi-GB content moves (design section 4). */
    std::stable_sort(task->contents.begin(), task->contents.end(),
                     [](const TaskContent &lhs, const TaskContent &rhs) {
                         return lhs.kind == ContentKind::Meta && rhs.kind != ContentKind::Meta;
                     });
    return true;
}

SkipDecision DecideSkip(const InstallOptions &options, std::uint32_t package_version,
                        std::uint32_t installed_version, bool has_this_version) {
    /* "This exact version is already here" is one question, asked by the console (see
       GameInstall::Prepare): the meta database and the application record both have it. */
    if (options.skip_if_already_installed && has_this_version) {
        return SkipDecision::AlreadyInstalled;
    }
    if (!options.allow_downgrade && installed_version > package_version) {
        return SkipDecision::NewerInstalled;
    }
    return SkipDecision::Install;
}

std::uint64_t ApplicationIdOf(std::uint64_t title_id, MetaType type) {
    if (type == MetaType::Patch) {
        return title_id ^ 0x800ULL;
    }
    if (type == MetaType::AddOnContent) {
        return (title_id ^ 0x1000ULL) & ~0xFFFULL;
    }
    return title_id;
}

}  // namespace acnh_manager::install
