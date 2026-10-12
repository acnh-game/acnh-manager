#pragma once

/* The two services the installer needs that libnx does not wrap, plus the ones it does.

   libnx (4.12.0) has no `es` at all and no `nsPushApplicationRecord`, so those two calls are
   raw IPC here.  The command ids and buffer layouts come from the reference implementation
   (`yati/nx/es.cpp`, `yati/nx/ns.cpp`), which is the same GPLv3 project family and was checked
   against real installs:

     es  cmd 1   ImportTicket(tik, cert)          two map-alias input buffers
     es  cmd 11  ListCommonTicket(count)          one map-alias output buffer
     ns  cmd 16  PushApplicationRecord(event, tid, records[N])   one map-alias input buffer
     ns  cmd 17  ListApplicationRecordContentMeta(offset, tid) -> entries read
     ns  cmd 27  DeleteApplicationRecord(tid)

   Everything a caller has to know about them (why the record type is 0x3, what a
   ContentStorageRecord is) is in the implementation. */

#include <switch.h>

#include <cstdint>
#include <string>
#include <vector>

namespace acnh_manager::install {

/* One row of the console's application record: the content meta key plus the storage it lives
   on.  This is what `PushApplicationRecord` takes, and what `ListApplicationRecordContentMeta`
   returns. */
struct ContentStorageRecord {
    NcmContentMetaKey key{};
    std::uint8_t storage_id{0}; /* NcmStorageId */
    std::uint8_t padding[7]{};
};

class NxServices {
public:
    NxServices() = default;
    ~NxServices();
    NxServices(const NxServices &) = delete;
    NxServices &operator=(const NxServices &) = delete;

    /* Opens everything once (`es`, and `ns`'s application-manager session).  Safe to call twice. */
    bool Open(std::string *error);
    void Close();

    /* es: import a ticket.  `ticket_imported` is the "already there" answer -- importing twice is
       harmless, so a failure is only reported when the ticket really did not land. */
    bool ImportTicket(const void *tik, std::size_t tik_size, const void *cert, std::size_t cert_size,
                      std::string *error);

    /* es: how many common tickets the console has (a cheap liveness check for the log). */
    bool CountCommonTickets(int *count, std::string *error);

    /* ns: tell the system that `title_id` is installed, with `records` as its content set.  This
       is the step that makes the home screen and the album see the game; without it the content
       is on the card but nothing knows about it. */
    bool PushApplicationRecord(std::uint64_t title_id,
                               const std::vector<ContentStorageRecord> &records,
                               std::string *error);

    /* ns: the records currently registered for `title_id` (all storage ids). */
    bool ListApplicationRecords(std::uint64_t title_id, std::vector<ContentStorageRecord> *out,
                                std::string *error);

    /* ns: forget a title entirely (used when a rollback deletes what we registered). */
    bool DeleteApplicationRecord(std::uint64_t title_id, std::string *error);

    /* avm: the system refuses to launch a title whose launch version is below the record's, so a
       fresh install has to raise it (HOS 6.0.0+; the reference implementation does the same). */
    bool PushLaunchVersion(std::uint64_t title_id, std::uint32_t version, std::string *error);

private:
    Service m_es{};
    Service m_ns_app{};
    bool m_es_open{false};
    bool m_ns_open{false};
    bool m_avm_open{false};
};

}  // namespace acnh_manager::install
