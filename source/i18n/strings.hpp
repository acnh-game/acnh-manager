#pragma once

/* UI text: the bilingual (Simplified Chinese / English) table.

   This is the one place where the source carries Chinese: the UI strings are product
   data, and the language rule in AGENTS.md allows Chinese for i18n.  Everything else is
   English.  Adding a string = add the id here, then fill both columns in strings.cpp
   (the host tests fail when a column is missing or the format specifiers disagree).

   Do not put comments between the enumerators: tools/check-i18n.py reads every non-blank line of
   this block as an id (that is how it compares the two orders), so a comment line counts as a
   string and the check fails with "enum has N ids".  Explanations belong above the enum. */

#include <cstddef>
#include <string>

namespace acnh_manager::i18n {

enum class Language { ZhHans, English };

enum class StringId {
    AppTitle,
    TabStatus,
    SectionInstall,
    LabelHos,
    LabelAppletMode,
    LabelGameRunning,
    LabelGameVersion,
    LabelContentId,
    LabelOverrideConfig,
    LabelLegacyCheat,
    LabelGateResult,
    LabelProblems,
    LanguageLabel,
    LanguageChinese,
    LanguageEnglish,
    ValueNotAvailable,
    ValueNone,
    ValuePresent,
    ValueAbsent,
    LabelPlan,
    LabelManifestSource,
    ResultSuccess,
    ResultFailure,
    ResultNoManifest,
    LabelUpdateCheck,
    ValueNotChecked,
    UpdateCheckOk,
    UpdateCheckFailed,
    SubUpdateChecking,
    SubUpdateUpToDate,
    SubUpdateNewer,
    SubUpdateFailed,
    SubUpdatePublished,
    SubUpdateNewerUnsupported,
    ProgressInstalling,
    ProgressUninstalling,
    ProgressNote,
    LabelProgressFile,
    GuideTitle,
    GuideWhat,
    GuideHow,
    GuideHowMore,
    GuideWhere,
    GuideScan,
    TabGuide,
    GuideMark,
    UpdateShortOffline,
    UpdateShortHttp,
    UpdateShortSignature,
    UpdateShortManifest,
    UpdateShortInternal,
    ValueEmbeddedManifest,
    ValueNetworkManifest,
    ValueAgentCarried,
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
    StateGameMissing,
    StateUnsupported,
    StateFailed,
    StateRepair,
    StateIncomplete,
    StateNotInstalled,
    StateUpdateAvailable,
    StateInstalled,
    StateGameMissingSub,
    StateUnsupportedSub,
    HomeVersions,
    HomeVersionsAvailable,
    HomeVersionsUpdate,
    ValueNotInstalled,
    BtnInstall,
    BtnUpdate,
    BtnUpToDate,
    BtnRepair,
    BtnRetry,
    BtnRecheck,
    BtnCheckUpdate,
    BtnUninstall,
    SubInstall,
    SubRepair,
    SubRetry,
    SubUpToDate,
    SubCheckUpdate,
    SubRecheck,
    SubUninstall,
    HeadSubtitle,
    BtnUnavailable,
    SectionAdvanced,
    LabelEmbeddedAgent,
    LabelInstallRecord,
    LabelLogPath,
    ValueInstallRecord,
    ValueNoInstallRecord,
    TabDetails,
    LabelExit,
    LabelBack,
    AppletModeLibrary,
    ConfirmInstallNote,
    ConfirmUninstallNote,
    ConfirmWillInstall,
    ConfirmStart,
    ConfirmRemove,
    Done,
    AppletModeApplication,
    ResultUninstallOk,
    ResultUninstallFailed,
    LabelPlanRemove,
    LabelPlanMore,
    UninstallFailed,
    ResultReadyNote,
    ResultRelinkHint,
    HomeLegacyCheat,
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
