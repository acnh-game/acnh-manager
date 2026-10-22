#include "install/package.hpp"

#include <algorithm>
#include <cstring>


namespace acnh_manager::install {
namespace {

/* CNMT layout (the payload `ReadPackage` leaves in PackageInfo::meta_blob).  The header the
   console's own record starts with sits at 0x0E inside it, after the title id, the version, the
   meta type and the platform byte. */
constexpr std::size_t kCnmtHeaderOffset = 0x0E;
constexpr std::size_t kCnmtHeaderSize = 0x20;       /* title id .. required system version */
constexpr std::size_t kCnmtContentEntrySize = 0x38; /* 0x20 sha256 + 0x18 content info */
constexpr std::size_t kContentInfoSize = 0x18;
constexpr std::size_t kMetaInfoSize = 0x10;

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* The .nca file of a content, by the name the container gives it. */
const ContainerEntry *FindContentEntry(const ContainerInfo &container, const std::string &id) {
    for (const auto &entry : container.contents) {
        /* Both names stand for the same content: `<id>.nca` (plain) and `<id>.ncz` (an NCZ stream,
           which is what an NSZ/XCZ carries for the NCAs that compress).  The extra shapes the
           meta content uses (`<id>.cnmt.nca`) pass the same test, because their first dot is the
           one after the id. */
        if (entry.name.compare(0, id.size(), id) == 0 && entry.name.find('.') == id.size() &&
            (EndsWith(entry.name, ".nca") || EndsWith(entry.name, ".ncz"))) {
            return &entry;
        }
    }
    return nullptr;
}

std::uint16_t ReadU16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void WriteU16(std::vector<std::uint8_t> *out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

/* 32 hex digits -> the 16 bytes they spell.  Content ids are written in a file name and in the
   record in the same order (checked against a real package: every id the CNMT lists appears in
   the container's file names character for character). */
bool IdFromHex(const std::string &hex, std::uint8_t *out) {
    if (hex.size() < 32) {
        return false;
    }
    for (std::size_t i = 0; i < 16; ++i) {
        unsigned value = 0;
        for (int half = 0; half < 2; ++half) {
            const char c = hex[i * 2 + static_cast<std::size_t>(half)];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<unsigned>(c - 'a') + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<unsigned>(c - 'A') + 10;
            } else {
                return false;
            }
            value = (value << 4) | digit;
        }
        out[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

/* One 24-byte NcmContentInfo: id, 40-bit size, attribute, content type, id offset. */
void AppendContentInfo(std::vector<std::uint8_t> *out, const std::uint8_t *id,
                       std::uint64_t size, std::uint8_t type) {
    out->insert(out->end(), id, id + 16);
    out->push_back(static_cast<std::uint8_t>(size & 0xFF));
    out->push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((size >> 16) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((size >> 24) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((size >> 32) & 0xFF));
    out->push_back(0);    /* attribute */
    out->push_back(type); /* NcmContentType */
    out->push_back(0);    /* id offset: only delta fragments use it */
}

}  // namespace

bool BuildMetaRecordBlob(const std::vector<std::uint8_t> &cnmt_payload,
                         const std::string &meta_content_name, const ContainerInfo &container,
                         bool lower_required_system_version, MetaRecordShape shape,
                         std::vector<std::uint8_t> *out, std::string *error) {
    out->clear();
    const std::vector<std::uint8_t> &payload = cnmt_payload;
    if (payload.size() < kCnmtHeaderSize) {
        *error = "the CNMT payload is too small to describe a record";
        return false;
    }
    const std::uint16_t extended_header_size = ReadU16(payload.data() + kCnmtHeaderOffset);
    const std::uint16_t content_count = ReadU16(payload.data() + kCnmtHeaderOffset + 2);
    const std::uint16_t content_meta_count = ReadU16(payload.data() + kCnmtHeaderOffset + 4);
    const std::uint8_t attributes = payload[kCnmtHeaderOffset + 6];
    const std::uint8_t storage_id = payload[kCnmtHeaderOffset + 7];
    const std::uint8_t meta_type = payload[kCnmtHeaderOffset - 2];
    const std::size_t extended = kCnmtHeaderSize + extended_header_size;
    const std::size_t entries_end =
        extended + static_cast<std::size_t>(content_count) * kCnmtContentEntrySize;
    const std::size_t metas_end =
        entries_end + static_cast<std::size_t>(content_meta_count) * kMetaInfoSize;
    if (metas_end > payload.size()) {
        *error = "the CNMT content table runs past the end of the payload";
        return false;
    }
    const ContainerEntry *meta = nullptr;
    for (const auto &entry : container.contents) {
        if (entry.name == meta_content_name) {
            meta = &entry;
            break;
        }
    }
    if (meta == nullptr) {
        *error = "the container does not carry the meta content it names";
        return false;
    }

    std::vector<std::uint8_t> blob;
    blob.reserve(8 + extended_header_size + (1 + static_cast<std::size_t>(content_count)) *
                                                          kContentInfoSize +
                 static_cast<std::size_t>(content_meta_count) * kMetaInfoSize);
    /* The header the console stores, with this package's meta content counted as a content. */
    WriteU16(&blob, extended_header_size);
    /* The count has to be the number of entries written below, and that is *not* the CNMT's own
       content_count: the delta fragments are dropped (see the loop), so the header is patched
       after them rather than before.  Reserve its place here. */
    const std::size_t count_offset = blob.size();
    WriteU16(&blob, 0);
    WriteU16(&blob, content_meta_count);
    blob.push_back(attributes);
    blob.push_back(shape == MetaRecordShape::Forwarder ? 0 : storage_id);
    blob.insert(blob.end(), payload.begin() + kCnmtHeaderSize, payload.begin() + extended);
    /* The firmware version the title asks for sits 8 bytes into the extended header of an
       application or a patch ({u64 id, u32 required_system_version, ...}), and the reference
       implementation zeroes it by default (`lower_system_version`).  Installing a package built for
       newer firmware otherwise leaves a record the console reads as "a system update is required"
       and refuses to launch, while the same package installs and runs with the field cleared --
       which is why this has to match.  It only touches what the console stores, not the contents. */
    constexpr std::uint8_t kMetaTypeApplication = 0x80;
    constexpr std::uint8_t kMetaTypePatch = 0x81;
    constexpr std::size_t kRecordHeaderSize = 8; /* the blob's own header, before the extended one */
    constexpr std::size_t kRequiredSystemVersionAt = 8;
    if (lower_required_system_version &&
        (meta_type == kMetaTypeApplication || meta_type == kMetaTypePatch) &&
        extended_header_size >= kRequiredSystemVersionAt + 4) {
        const std::size_t at = kRecordHeaderSize + kRequiredSystemVersionAt;
        blob[at + 0] = 0;
        blob[at + 1] = 0;
        blob[at + 2] = 0;
        blob[at + 3] = 0;
    }
    /* The meta content first: it is what the console reads to reach the rest of the title. */
    std::uint8_t meta_id[16] = {};
    if (!IdFromHex(meta->name, meta_id)) {
        *error = "the meta content's name is not a content id";
        return false;
    }
    AppendContentInfo(&blob, meta_id,
                      shape == MetaRecordShape::Forwarder ? payload.size() : meta->size,
                      0); /* 0 = NcmContentType_Meta */
    std::uint16_t written_contents = 0;
    for (std::uint16_t i = 0; i < content_count; ++i) {
        const std::size_t entry =
            extended + static_cast<std::size_t>(i) * kCnmtContentEntrySize;
        /* The delta fragments are *listed* by the CNMT but not shipped inside the package (we do
           not fetch them, and neither does the reference implementation), so they must not be
           listed by the record either: the record is the console's answer to "which contents make
           up this version", and every entry in it has to exist in the storage.  Listing them
           anyway is what made an update whose deltas were gone (after DBI deleted the console's
           other packages) impossible to launch: the record claimed 33 contents, three were there,
           and the system refused the title.  The reference skips exactly these entries when it
           builds the same blob (`yati.cpp`: `if (info.content_type == NcmContentType_DeltaFragment)
           continue;`, then `content_count = infos.size() + 1`). */
        if (payload[entry + 0x20 + 0x16] == 0x06) { /* NcmContentType_DeltaFragment */
            continue;
        }
        blob.insert(blob.end(), payload.begin() + entry + 0x20,
                    payload.begin() + entry + 0x20 + kContentInfoSize);
        ++written_contents;
    }
    blob[count_offset] = static_cast<std::uint8_t>((written_contents + 1) & 0xFF);
    blob[count_offset + 1] = static_cast<std::uint8_t>(((written_contents + 1) >> 8) & 0xFF);
    blob.insert(blob.end(), payload.begin() + entries_end, payload.begin() + metas_end);
    *out = std::move(blob);
    return true;
}

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
        /* Both spellings: a container that stores its meta content compressed (an NSZ/XCZ made by
           a tool that compresses more than the reference's own dumper does) names it `.cnmt.ncz`,
           and the reference implementation looks for both. */
        if (EndsWith(entry.name, ".cnmt.nca") || EndsWith(entry.name, ".cnmt.ncz")) {
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
       already has the ticket when it matters).  A ticket is only usable next to its certificate,
       and both are named by the rights id they belong to -- the reference implementation pairs
       them the same way and refuses a `.tik` whose `.cert` is missing. */
    for (const auto &entry : out->container.extra) {
        if (!EndsWith(entry.name, ".tik") || entry.name.size() < 32) {
            continue;
        }
        const std::string rights_id = entry.name.substr(0, 32);
        const ContainerEntry *cert = nullptr;
        for (const auto &candidate : out->container.extra) {
            if (candidate.name == rights_id + ".cert") {
                cert = &candidate;
                break;
            }
        }
        if (cert == nullptr) {
            *error = "the package carries a ticket without its certificate: " + entry.name;
            return false;
        }
        PackageTicket pair;
        pair.rights_id = rights_id;
        pair.ticket.resize(static_cast<std::size_t>(entry.size));
        pair.certificate.resize(static_cast<std::size_t>(cert->size));
        if (!reader.Read(entry.offset, pair.ticket.size(), pair.ticket.data())) {
            *error = "cannot read the ticket";
            return false;
        }
        if (!reader.Read(cert->offset, pair.certificate.size(), pair.certificate.data())) {
            *error = "cannot read the certificate";
            return false;
        }
        out->tickets.push_back(std::move(pair));
    }
    return true;
}

bool FillTaskContents(const std::vector<CnmtInfo> &metas, const ContainerInfo &container,
                      InstallTask *task, std::string *error) {
    task->contents.clear();
    std::size_t total = 0;
    for (const CnmtInfo &meta : metas) {
        total += meta.contents.size();
    }
    task->contents.reserve(total);
    for (const CnmtInfo &meta : metas) {
        for (const auto &content : meta.contents) {
            if (content.type == ContentType::DeltaFragment) {
                /* Delta fragments are listed by the CNMT but not shipped inside the package: they
                   are the "what changed since the version you might have" data, and the console
                   does not need them from us.  The reference implementation skips them for the
                   same reason -- without this, every update package would look like it is missing
                   a content. */
                continue;
            }
            /* Shared between metas (an update reusing the base game's data) and already listed by
               an earlier meta: one entry is enough, and fetching it twice would be wasted work. */
            bool seen = false;
            for (const TaskContent &existing : task->contents) {
                if (existing.content_id == content.content_id) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            const ContainerEntry *entry = FindContentEntry(container, content.content_id);
            if (entry == nullptr) {
                *error = "the package lists a content it does not carry: " + content.content_id;
                return false;
            }
            TaskContent item;
            item.name = entry->name;
            item.offset = entry->offset;
            item.size = entry->size;
            item.packed_size = entry->packed_size;
            item.compressed = entry->compressed;
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
    }
    /* The meta content first: it carries the CNMT, so reading it early is what lets the console
       answer "is this already installed?" before the multi-GB content moves (design section 4). */
    std::stable_sort(task->contents.begin(), task->contents.end(),
                     [](const TaskContent &lhs, const TaskContent &rhs) {
                         return lhs.kind == ContentKind::Meta && rhs.kind != ContentKind::Meta;
                     });
    return true;
}

SkipDecision DecideSkip(const InstallOptions &options, MetaType type, std::uint32_t package_version,
                        std::uint32_t installed_version, bool has_this_version) {
    /* "This exact version is already here" is one question, asked by the console (see
       GameInstall::Prepare): the meta database and the application record both have it. */
    if (options.skip_if_already_installed && has_this_version) {
        return SkipDecision::AlreadyInstalled;
    }
    if (type == MetaType::Patch && !options.allow_downgrade && installed_version > package_version) {
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
