#pragma once

/* NCA header and CNMT parsing: the two structures that decide what a package *is* (which game,
   which version, base/update/DLC) and what has to be installed and where.

   Both parsers are pure: the caller hands them bytes and gets a struct.  Decrypting the NCA
   header (the spl header key) and reading the CNMT payload out of the meta content are device
   work and live in the engine; everything that can be decided from the bytes is decided here,
   so the host tests can pin it down with hand-built fixtures.

   Layouts (nxdumptool's and libnx's definitions agree with the reference implementation):

     NcaHeader   0x000 rsa fixed key, 0x100 rsa npdm, 0x200 magic/distribution/content type,
                 0x208 size, 0x210 program id, 0x220 key generation, 0x240 section table,
                 0x400 the four file-system headers (the header is 0xC00 bytes in total)
     Cnmt        0x00 title id, 0x08 version, 0x0C type, 0x0E extended header size,
                 0x10 content count, 0x12 content meta count, 0x14 attributes/storage id,
                 0x20 extended header, then the content infos (0x38 each), then the meta infos
                 (0x10 each).  The extended header's first fields depend on the type. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acnh_manager::install {

/* NcmContentMetaType -- the same numbers the system uses. */
enum class MetaType {
    SystemProgram = 0x01,
    SystemData = 0x02,
    SystemUpdate = 0x03,
    BootImagePackage = 0x04,
    BootImagePackageSafe = 0x05,
    Application = 0x80,
    Patch = 0x81,
    AddOnContent = 0x82,
    Delta = 0x83,
    DataPatch = 0x84,
    Unknown = 0xFF,
};

/* NcmContentType. */
enum class ContentType {
    Program = 0x00,
    Meta = 0x01,
    Control = 0x02,
    Manual = 0x03,
    Data = 0x04,
    PublicData = 0x05,
    DeltaFragment = 0x06,
    Unknown = 0xFF,
};

/* What the player sees on the confirm page: 本体 / 更新 / DLC. */
const char *MetaTypeLabel(MetaType type);

/* True for the two kinds whose version is the one the agent's gate asks about: the game itself
   and its update.  An add-on (DLC) and a data patch leave that version where it was, so a note
   about "this version works with the agent" describes nothing they touch -- which is how
   installing an add-on could warn that the agent does not support "this version". */
bool MetaTypeCarriesGameVersion(MetaType type);

struct CnmtContent {
    std::string content_id; /* 32 hex digits, lower case */
    ContentType type{ContentType::Unknown};
    std::uint64_t size{0};
};

struct CnmtInfo {
    std::string title_id; /* 16 hex digits, lower case (the id inside the CNMT, not the file name) */
    std::uint32_t version{0};
    MetaType type{MetaType::Unknown};
    std::uint16_t attributes{0};
    std::uint8_t storage_id{0};
    std::uint32_t required_system_version{0}; /* 0 when the extended header does not carry one */
    /* What the parse saw, for the log line: a first content id that does not match any .nca name
       in the container means the entry table is misaligned and every type byte is noise. */
    std::uint16_t extended_header_size{0};
    std::uint16_t content_meta_count{0};
    /* Filled by the parser for the log line: the first few entries as read, so a field-offset
       mistake is visible instead of inferred. */
    std::string parse_note;
    std::string application_id;               /* patch/DLC: the base game this belongs to */
    std::vector<CnmtContent> contents;
};

/* Parses the CNMT (the payload of the meta content).  `size` is the whole payload as read. */
bool ParseCnmt(const std::uint8_t *data, std::size_t size, CnmtInfo *out, std::string *error);

struct NcaHeaderInfo {
    std::string content_id; /* from the caller: the header does not carry it, the name does */
    std::uint32_t magic{0};
    std::uint8_t distribution_type{0}; /* 0 = system (SD/NAND launchable), 1 = gamecard */
    ContentType content_type{ContentType::Unknown};
    std::uint8_t key_generation{0}; /* the higher of old/new key generation */
    std::uint64_t size{0};
    std::string program_id; /* 16 hex digits, lowercase */
    /* Section 0's byte range inside the NCA, straight from the section table (the values in the
       header are in 0x200 units).  Reading the section table instead of assuming "the first
       section starts at 0xC00" is what the meta content needs: on hardware that assumption put
       the CNMT read in the middle of the file system header. */
    std::uint64_t section0_offset{0};
    std::uint64_t section0_size{0};
    bool is_gamecard() const { return distribution_type != 0; }
};

/* Parses a *decrypted* 0xC00-byte NCA header.  Only NCA3 is accepted: it is the format every
   package in circulation uses, and accepting an older magic would silently change the key
   handling. */
bool ParseNcaHeader(const std::uint8_t *data, std::size_t size, NcaHeaderInfo *out,
                    std::string *error);

/* The distribution bit rewrite an XCI base needs: gamecard content has to become system content
   before the console will launch it from SD or NAND (design section 5, `ignore_distribution_bit`
   is false by default, which is what turns this on). */
void SetDistributionToSystem(std::uint8_t *decrypted_header);

/* 16 bytes -> 32 lower-case hex digits, and back.  Content and title ids are compared as text
   everywhere else in the app (the manifest, the state file, the log), so this module keeps the
   same shape. */
std::string HexId(const std::uint8_t *bytes, std::size_t size);

}  // namespace acnh_manager::install
