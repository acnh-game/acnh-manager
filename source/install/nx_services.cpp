#include "install/nx_services.hpp"

#include <cstring>

#include "install/content_install.hpp" /* ToHex: the same hex spelling the trace lines use */

namespace acnh_manager::install {
namespace {

/* NsApplicationRecordType: a title that finished installing.  Other values describe a running,
   downloading or archived title; 0x3 is the one an installer writes. */
constexpr std::uint8_t kApplicationRecordTypeInstalled = 0x3;

/* es command ids (see the header).  `es` is opened by name: libnx ships no wrapper for it. */
constexpr std::uint32_t kEsImportTicket = 1;
constexpr std::uint32_t kEsCountCommonTicket = 9;
constexpr std::uint32_t kEsListCommonTicket = 11;

/* ns: IApplicationManagerInterface, reached through nsGetApplicationManagerInterface(). */
constexpr std::uint32_t kNsPushApplicationRecord = 16;
constexpr std::uint32_t kNsListApplicationRecordContentMeta = 17;
constexpr std::uint32_t kNsDeleteApplicationRecord = 27;
/* Not in libnx: the same interface handles this by command id. */
constexpr std::uint32_t kNsInvalidateApplicationControlCache = 404;

std::string Describe(const char *what, Result rc) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08x)", what, static_cast<unsigned>(rc));
    return buffer;
}

}  // namespace

NxServices::~NxServices() { Close(); }

bool NxServices::Open(std::string *error) {
    if (!m_es_open) {
        const Result rc = smGetService(&m_es, "es");
        if (R_FAILED(rc)) {
            *error = Describe("opening the es service", rc);
            return false;
        }
        m_es_open = true;
    }
    if (!m_ns_open) {
        Result rc = nsInitialize();
        if (R_FAILED(rc)) {
            *error = Describe("opening the ns service", rc);
            return false;
        }
        /* HOS 3.0.0+ hands out a separate session for the application manager; older systems
           expose it on the main one.  The installer only runs on systems the app supports, but
           the fallback costs one line and keeps the code honest about what it does. */
        if (hosversionAtLeast(3, 0, 0)) {
            rc = nsGetApplicationManagerInterface(&m_ns_app);
        } else {
            m_ns_app = *nsGetServiceSession_ApplicationManagerInterface();
            rc = 0;
        }
        if (R_FAILED(rc)) {
            *error = Describe("getting the ns application manager", rc);
            return false;
        }
        m_ns_open = true;
    }
    if (!m_avm_open) {
        const Result rc = avmInitialize();
        if (R_FAILED(rc)) {
            *error = Describe("opening the avm service", rc);
            return false;
        }
        m_avm_open = true;
    }
    return true;
}

void NxServices::Close() {
    if (m_avm_open) {
        avmExit();
        m_avm_open = false;
    }
    if (m_ns_open) {
        serviceClose(&m_ns_app);
        nsExit();
        m_ns_open = false;
    }
    if (m_es_open) {
        serviceClose(&m_es);
        m_es_open = false;
    }
}

bool NxServices::ImportTicket(const void *tik, std::size_t tik_size, const void *cert,
                             std::size_t cert_size, std::string *error) {
    if (tik == nullptr || tik_size == 0 || cert == nullptr || cert_size == 0) {
        *error = "the package's ticket or certificate is missing";
        return false;
    }
    const Result rc = serviceDispatch(&m_es, kEsImportTicket,
                                      .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                                                       SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                                      .buffers = {{tik, tik_size}, {cert, cert_size}});
    if (R_FAILED(rc)) {
        *error = Describe("importing the ticket", rc);
        return false;
    }
    return true;
}

bool NxServices::CountCommonTickets(int *count, std::string *error) {
    s32 value = 0;
    const Result rc = serviceDispatchOut(&m_es, kEsCountCommonTicket, value);
    if (R_FAILED(rc)) {
        *error = Describe("counting common tickets", rc);
        return false;
    }
    *count = value;
    return true;
}

bool NxServices::PushApplicationRecord(std::uint64_t title_id,
                                       const std::vector<ContentStorageRecord> &records,
                                       std::string *error) {
    if (records.empty()) {
        *error = "no content records to push";
        return false;
    }
    const struct {
        std::uint8_t last_modified_event;
        std::uint8_t padding[0x7];
        std::uint64_t title_id;
    } in = {kApplicationRecordTypeInstalled, {0}, title_id};

    const Result rc =
        serviceDispatchIn(&m_ns_app, kNsPushApplicationRecord, in,
                          .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                          .buffers = {{records.data(),
                                       records.size() * sizeof(ContentStorageRecord)}});
    if (R_FAILED(rc)) {
        *error = Describe("writing the application record", rc);
        return false;
    }
    return true;
}

bool NxServices::ListApplicationRecords(std::uint64_t title_id,
                                        std::vector<ContentStorageRecord> *out,
                                        std::string *error) {
    out->clear();
    s32 total = 0;
    Result rc = nsCountApplicationContentMeta(title_id, &total);
    if (R_FAILED(rc)) {
        *error = Describe("counting the installed content meta", rc);
        return false;
    }
    if (total <= 0) {
        return true;
    }
    std::vector<ContentStorageRecord> records(static_cast<std::size_t>(total));
    s32 written = 0;
    const struct {
        std::uint64_t offset;
        std::uint64_t title_id;
    } in = {0, title_id};
    rc = serviceDispatchInOut(&m_ns_app, kNsListApplicationRecordContentMeta, in, written,
                              .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                              .buffers = {{records.data(),
                                           records.size() * sizeof(ContentStorageRecord)}});
    if (R_FAILED(rc)) {
        *error = Describe("listing the application records", rc);
        return false;
    }
    records.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    *out = std::move(records);
    return true;
}

bool NxServices::InvalidateApplicationControlCache(std::uint64_t title_id, std::string *error) {
    const Result rc = serviceDispatchIn(&m_ns_app, kNsInvalidateApplicationControlCache, title_id);
    if (R_FAILED(rc)) {
        *error = Describe("invalidating the application control cache", rc);
        return false;
    }
    return true;
}

bool NxServices::DeleteApplicationRecord(std::uint64_t title_id, std::string *error) {
    const Result rc = serviceDispatchIn(&m_ns_app, kNsDeleteApplicationRecord, title_id);
    if (R_FAILED(rc)) {
        *error = Describe("deleting the application record", rc);
        return false;
    }
    return true;
}

bool NxServices::PushLaunchVersion(std::uint64_t title_id, std::uint32_t version,
                                   std::string *error) {
    const Result rc = avmPushLaunchVersion(title_id, version);
    if (R_FAILED(rc)) {
        *error = Describe("recording the launch version", rc);
        return false;
    }
    return true;
}

bool NxServices::GetIcon(std::uint64_t application_id, std::vector<std::uint8_t> *icon,
                         std::string *error) {
    icon->clear();
    if (!m_ns_open) {
        *error = "no control data (the ns service is not open)";
        return false;
    }
    /* The answer is NACP + icon, so the buffer has to cover the whole declaration: `ns` maps it
       as one out buffer and rejects a window that reaches past the end of a mapping.  144 KB is
       not stack material, and one static buffer is enough because this call is never concurrent
       (the installer runs on the thread that draws). */
    alignas(0x1000) static std::uint8_t control[sizeof(NsApplicationControlData)];
    u64 actual = 0;
    const Result rc = nsGetApplicationControlData(
        NsApplicationControlSource_Storage, application_id,
        reinterpret_cast<NsApplicationControlData *>(control), sizeof(control), &actual);
    if (R_FAILED(rc)) {
        *error = "no control data for this title (0x" + ToHex(&rc, sizeof(std::uint32_t)) + ")";
        return false;
    }
    constexpr std::size_t kIconOffset = offsetof(NsApplicationControlData, icon);
    constexpr std::size_t kIconSize = sizeof(NsApplicationControlData::icon);
    if (actual <= kIconOffset) {
        *error = "the control data carries no icon";
        return false;
    }
    const std::size_t available = (actual - kIconOffset) < kIconSize ? (actual - kIconOffset)
                                                                    : kIconSize;
    icon->assign(control + kIconOffset, control + kIconOffset + available);
    return true;
}

}  // namespace acnh_manager::install
