#pragma once

/* The running app's version, injected by the Makefile (`APP_VERSION` -> `ACNH_APP_VERSION`).

   Single source on purpose: the manifest gate validates `app.minVersion` against this string in
   four places (embedded manifest, dev manifest, update check, and the log/UI), and a hard-coded
   copy in any of them would silently keep validating against the version we no longer are. */

#ifndef ACNH_APP_VERSION
#define ACNH_APP_VERSION "0.0.0"
#endif

namespace acnh_manager {

inline constexpr const char *kAppVersion = ACNH_APP_VERSION;

}  // namespace acnh_manager
