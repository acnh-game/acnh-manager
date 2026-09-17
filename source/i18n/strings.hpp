#pragma once

/* UI text: the bilingual (Simplified Chinese / English) table.

   This is the one place where the source carries Chinese: the UI strings are product
   data, and the language rule in AGENTS.md allows Chinese for i18n.  Everything else is
   English.  Adding a string = add the id here, then fill both columns in strings.cpp
   (the host tests fail when a column is missing or the format specifiers disagree). */

#include <cstddef>
#include <string>

namespace acnh_manager::i18n {

enum class Language { ZhHans, English };

enum class StringId {
    AppTitle,
    TabStatus,
    TabSettings,
    SectionGame,
    SectionOverride,
    SectionInstall,
    SectionManifest,
    LabelHos,
    LabelAppletMode,
    LabelGameRunning,
    LabelGameVersion,
    LabelContentId,
    LabelModuleId,
    LabelOverrideConfig,
    LabelExistingFiles,
    LabelLegacyCheat,
    LabelManifestState,
    LabelGateResult,
    LabelProblems,
    HintControlsStatus,
    HintControlsSettings,
    LanguageLabel,
    LanguageChinese,
    LanguageEnglish,
    ValueNotAvailable,
    ValueGameNotRunning,
    ValueNone,
    ValuePresent,
    ValueAbsent,
    ManifestNotEmbedded,
    LegacyCheatWarning,
    RefreshHint,
    TabConfirm,
    TabResult,
    TabUninstall,
    LabelPlan,
    LabelPayloadDir,
    LabelDryRun,
    LabelManifestSource,
    ValueDryRunOn,
    ValueDryRunOff,
    ValueDevManifest,
    HintControlsConfirm,
    HintControlsResult,
    ConfirmInstall,
    ConfirmUninstall,
    ResultSuccess,
    ResultFailure,
    ResultNoManifest,
    UninstallIntro,
    LabelUpdateCheck,
    ValueNotChecked,
    UpdateCheckOk,
    UpdateCheckInvalid,
    UpdateCheckSkipped,
    UpdateCheckFailed,
    ValueEmbeddedManifest,
    OverrideAlwaysOn,
    OverrideNeverApplies,
    OverrideOnByDefault,
    OverrideOffByDefault,
    OverrideHbmenuNote,
    StateParseFailed,
    ManifestEmbeddedInvalid,
    ManifestInvalid,
    PayloadNotEmbedded,
    InstallErrPayloadRead,
    InstallErrPayloadSize,
    InstallErrPayloadSha,
    InstallErrWrite,
    InstallErrStateWrite,
    InstallErrStateParse,
    UninstallNoRecord,
    UninstallModified,
    UpdateErrMountSd,
    UpdateErrNoCaFile,
    UpdateErrNoCa,
    UpdateErrHttpsOnly,
    UpdateErrCurlInit,
    UpdateErrSocket,
    UpdateErrNetwork,
    UpdateOkFetched,
    ExitHint,
};

const char *Text(StringId id, Language language);

/* Look the entry up in the current language and format it printf-style.  The call site must
   pass the same conversions the table declares for that id.  The host tests prove the two
   language columns agree with each other; they cannot see the arguments at the call site,
   so keep both in sync by hand when adding a format string. */
std::string Format(StringId id, ...);

/* Number of table entries (tests use it to prove the data file matches the enum). */
unsigned StringCount();

/* Process-wide language.  The UI writes it when the user toggles; the install engine and
   the network layer read it so that user-visible messages they build stay localized. */
Language Current();
void SetLanguage(Language language);

}  // namespace acnh_manager::i18n
