#pragma once

/* 界面文案:简体中文 / English 双语表。
   约定:新增文案先加进 StringId,再在两列里都补上(缺一列会在测试里暴露)。 */

#include <cstddef>

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
    ExitHint,
};

const char *Text(StringId id, Language language);

/* 文案表项数(测试用:必须等于 StringId 的数量)。 */
unsigned StringCount();

}  // namespace acnh_manager::i18n
