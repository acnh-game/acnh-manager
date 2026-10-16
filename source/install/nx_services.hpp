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

    /* ns: drop the cached control data (name/version/icon) for a title.  The system caches the
       NACP it serves, so without this the console keeps reporting the version it had before the
       install -- measured: contents written and record pushed, home screen still said 1.0.0. */
    bool InvalidateApplicationControlCache(std::uint64_t title_id, std::string *error);

    /* ns: forget a title entirely (used when a rollback deletes what we registered). */
    bool DeleteApplicationRecord(std::uint64_t title_id, std::string *error);

    /* ns: the cover the console shows for a title, as the JPEG the developer shipped (256x256
       for every retail title).  This is the console's own answer -- the same call the home screen
       makes -- so a package whose game is not installed simply has none.  A missing cover is
       never an error the player has to see: the caller draws nothing and carries on. */
    bool GetIcon(std::uint64_t application_id, std::vector<std::uint8_t> *icon, std::string *error);

    /* ns: the version string the console shows for a title ("3.0.3"), from the same control data
       the cover comes from.  The title version *number* is Nintendo's own build id -- 3.0.3 is
       0x220000 -- so it is not a version anyone can read, and this string is.  Empty when the
       console has no control data to answer with. */
    bool GetDisplayVersion(std::uint64_t application_id, std::string *version, std::string *error);

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

/* True when the console has an IP address.  Both ways of getting a download address need a
   network -- fetching one, and serving the browser page that can submit one -- so the installer
   asks before it offers either, and the answer is worth a line in the log.  False with an error
   when the network service cannot be opened at all (which is also "no address to show"). */
bool HasNetworkAddress(bool *out, std::string *error);

/* The console's own address as text ("192.168.8.160"), which is what the install page puts in the
   address the player types into a browser and in the QR code.  Empty with an error when there is no
   address (or no network service): the screens then say "connect first" instead of showing a
   half-built address. */
bool CurrentIpAddressText(std::string *out, std::string *error);

}  // namespace acnh_manager::install
