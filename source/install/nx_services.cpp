#include "install/nx_services.hpp"

#include <cstring>

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

}  // namespace acnh_manager::install
