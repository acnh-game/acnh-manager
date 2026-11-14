#pragma once

#include <array>
#include <cstdint>

namespace acnh_manager::env {

/* User content can live either in the console's built-in user partition or on the SD card.
   These values match NcmStorageId_BuiltInUser and NcmStorageId_SdCard, but the selection
   logic stays free of libnx so it can be host-tested. */
inline constexpr std::uint8_t kUserStorageBuiltIn = 4;
inline constexpr std::uint8_t kUserStorageSdCard = 5;
inline constexpr std::uint32_t kNcmContentMetaNotFound = 0x00000E05;

struct NcmMetaLookupPlan {
    std::array<std::uint8_t, 2> storages{};
    std::uint8_t count{0};
    bool update_absent{false};
};

/* The ns content table is authoritative when it names the storage holding the game's Patch.
   Query that database first because updates purchased on a stock console commonly live in the
   built-in user partition rather than on the SD card.  Without a useful ns answer, check both
   possible user-storage databases. */
constexpr NcmMetaLookupPlan PlanNcmMetaLookup(bool meta_list_ok, bool patch_found,
                                              std::uint8_t patch_storage) {
    NcmMetaLookupPlan plan;
    plan.update_absent = meta_list_ok && !patch_found;
    if (patch_found && patch_storage != 0) {
        plan.storages[0] = patch_storage;
        plan.count = 1;
        return plan;
    }
    plan.storages[0] = kUserStorageBuiltIn;
    plan.storages[1] = kUserStorageSdCard;
    plan.count = 2;
    return plan;
}

constexpr bool IsExpectedNcmMiss(bool update_absent, std::uint32_t rc) {
    return update_absent && rc == kNcmContentMetaNotFound;
}

constexpr bool ShouldReportNcmLookupFailure(bool update_absent, bool unexpected_failure) {
    return !update_absent || unexpected_failure;
}

}  // namespace acnh_manager::env
