#pragma once

/* Install options: the same list the reference implementation calls its "install options", with
   the same defaults (include/app.hpp).  1.1.0 ships no UI for them -- the engine carries one of
   these structs with the defaults below, so adding a list screen later is a UI change only.

   The two defaults that decide what this app can install are the verification ones:
   `skip_nca_hash_verify` and `skip_rsa_header_fixed_key_verify` are **true**, exactly as in the
   reference implementation, which is why repacked NSPs and NSP<->XCI converts install fine.
   That is a deliberate copy of its behaviour, not an oversight; see
   docs/game_installer_design.md section 5. */

#include <string>

namespace acnh_manager::install {

/* Where the content goes.  The reference implementation keeps three booleans; one enum is
   enough here because our UI only ever offers "SD" and, when SD is full, "the console". */
enum class TargetStorage {
    SdCard,
    Nand,
};

struct InstallOptions {
    /* Target: SD by default; the console's internal memory only when the user says so after a
       failed space check.  (emuMMC is not offered: this app is not an emuMMC tool, and the
       reference implementation's own default is off.) */
    TargetStorage target{TargetStorage::SdCard};

    /* Content selection: all five kinds are installed; the skips exist because the reference
       implementation exposes them. */
    bool skip_base{false};
    bool skip_patch{false};
    bool skip_addon{false};
    bool skip_data_patch{false};
    bool skip_ticket{false};

    /* Idempotence and downgrade. */
    bool skip_if_already_installed{true};
    bool allow_downgrade{false};

    /* Verification (see the note above). */
    bool skip_nca_hash_verify{true};
    bool skip_rsa_header_fixed_key_verify{true};
    bool skip_rsa_npdm_fixed_key_verify{true};

    /* Rewrites: the distribution bit turns gamecard content into something the console is
       willing to launch from SD/NAND, which is what an XCI base needs.  The three [needs keys]
       conversions are only taken when the content really requires them. */
    bool ignore_distribution_bit{false};
    /* Off, because this app never converts a ticket: the package's ticket is imported as it is
       (design section 5, and `docs/device-acceptance.md`).  The flag only feeds MayNeedKeys(), so
       leaving it "on" said "this run may need prod.keys" about a run that never reads them. */
    bool convert_to_common_ticket{false};
    bool convert_to_standard_crypto{false};
    bool lower_master_key{false};
    bool lower_system_version{true};

    /* Only import the ticket, install no content (kept for parity with the reference UI). */
    bool ticket_only{false};

    /* True when the content may need /switch/prod.keys: the caller must then report a missing
       file as an error instead of silently skipping the conversion (design section 9). */
    bool MayNeedKeys() const {
        return convert_to_common_ticket || convert_to_standard_crypto || lower_master_key;
    }

    /* True when every content kind is skipped, which is never a meaningful install. */
    bool SkipsEverything() const {
        return skip_base && skip_patch && skip_addon && skip_data_patch;
    }

    /* Rules that keep a nonsensical combination from reaching the engine.  The UI cannot express
       these today, but the struct is public and the host tests pin the rules down. */
    bool Validate(std::string *error) const {
        if (ticket_only) {
            /* "Ticket only" means exactly that: the content skips have to be on as well, and the
               ticket itself must not be skipped -- otherwise the run does nothing at all. */
            if (!SkipsEverything()) {
                *error = "ticket-only means installing no content (the content skips must all be on)";
                return false;
            }
            if (skip_ticket) {
                *error = "ticket-only, but the ticket is skipped as well";
                return false;
            }
            return true;
        }
        if (SkipsEverything()) {
            *error = "every content kind is skipped, so there is nothing to install";
            return false;
        }
        return true;
    }
};

}  // namespace acnh_manager::install
