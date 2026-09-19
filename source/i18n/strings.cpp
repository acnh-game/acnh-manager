#include "strings.hpp"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace acnh_manager::i18n {
namespace {

struct Entry {
    const char *zh;
    const char *en;
};

constexpr Entry kStrings[] = {
    {"ACNH-Manager", "ACNH-Manager"},  /* AppTitle */
    {"状态", "Status"},  /* TabStatus */
    {"安装信息", "Install info"},  /* SectionInstall */
    {"系统版本", "System"},  /* LabelHos */
    {"运行模式", "Mode"},  /* LabelAppletMode */
    {"游戏是否运行", "Game running"},  /* LabelGameRunning */
    {"游戏版本号", "Game version"},  /* LabelGameVersion */
    {"更新包标识", "Update id"},  /* LabelContentId */
    {"覆盖键提示", "Override key advice"},  /* LabelOverrideConfig */
    {"旧金手指文件", "Legacy cheat file"},  /* LabelLegacyCheat */
    {"检测结果", "Check result"},  /* LabelGateResult */
    {"检测异常", "Detection problems"},  /* LabelProblems */
    {"界面语言", "Language"},  /* LanguageLabel */
    {"简体中文", "简体中文"},  /* LanguageChinese */
    {"English", "English"},  /* LanguageEnglish */
    {"(不可用)", "(unavailable)"},  /* ValueNotAvailable */
    {"无", "none"},  /* ValueNone */
    {"有", "present"},  /* ValuePresent */
    {"无", "absent"},  /* ValueAbsent */
    {"将要写入的文件", "Files to write"},  /* LabelPlan */
    {"安装包来源", "Install source"},  /* LabelManifestSource */
    {"安装成功", "Install succeeded"},  /* ResultSuccess */
    {"安装失败", "Install failed"},  /* ResultFailure */
    {"这个版本没有带安装包，请更新应用后重试", "This build has no installer bundled; update the app and try again"},  /* ResultNoManifest */
    {"联网检查", "Update check"},  /* LabelUpdateCheck */
    {"(未检查)", "(not checked)"},  /* ValueNotChecked */
    {"清单可用，agent %s", "manifest ok, agent %s"},  /* UpdateCheckOk */
    {"清单无效: %s", "invalid manifest: %s"},  /* UpdateCheckInvalid */
    {"跳过: %s", "skipped: %s"},  /* UpdateCheckSkipped */
    {"失败: %s", "failed: %s"},  /* UpdateCheckFailed */
    {"随应用自带", "bundled with the app"},  /* ValueEmbeddedManifest */
    {"覆盖始终生效(override_key 没有有效按键)", "Override always applies (override_key has no valid key)"},  /* OverrideAlwaysOn */
    {"覆盖永不生效(override_key 为空或按键名无法识别):请改成 !L 或删除该项", "Override never applies (override_key empty or the key name is unknown): change it to !L or remove the entry"},  /* OverrideNeverApplies */
    {"覆盖默认生效；启动游戏时不要按住 %s", "Override applies by default; do not hold %s while launching the game"},  /* OverrideOnByDefault */
    {"覆盖默认关闭；启动游戏时需要按住 %s", "Override is off by default; hold %s while launching the game"},  /* OverrideOffByDefault */
    {"。注意:按住 %s 启动游戏会进入 hbmenu", ". Note: holding %s while launching a game opens hbmenu"},  /* OverrideHbmenuNote */
    {"state.json 无法解析: %s", "state.json could not be parsed: %s"},  /* StateParseFailed */
    {"内嵌清单无效: %s", "invalid embedded manifest: %s"},  /* ManifestEmbeddedInvalid */
    {"清单无效: %s", "invalid manifest: %s"},  /* ManifestInvalid */
    {"内嵌 payload 里没有文件 \"%s\"", "embedded payload has no file \"%s\""},  /* PayloadNotEmbedded */
    {"payload 读取失败: %s", "payload read failed: %s"},  /* InstallErrPayloadRead */
    {"payload %s 大小不符: %llu != %llu", "payload %s size mismatch: %llu != %llu"},  /* InstallErrPayloadSize */
    {"payload %s sha256 不符: %s", "payload %s sha256 mismatch: %s"},  /* InstallErrPayloadSha */
    {"写入失败: %s", "write failed: %s"},  /* InstallErrWrite */
    {"写入 state.json 失败: %s", "writing state.json failed: %s"},  /* InstallErrStateWrite */
    {"state.json 解析失败: %s", "state.json parse failed: %s"},  /* InstallErrStateParse */
    {"没有安装记录，无需卸载", "nothing to uninstall (no install record)"},  /* UninstallNoRecord */
    {"只接受 https 地址", "only https URLs are accepted"},  /* UpdateErrHttpsOnly */
    {"curl_easy_init 失败", "curl_easy_init failed"},  /* UpdateErrCurlInit */
    {"socket 服务不可用(rc=0x%08X)", "socket service unavailable (rc=0x%08X)"},  /* UpdateErrSocket */
    {"网络失败: %s", "network failed: %s"},  /* UpdateErrNetwork */
    {"已获取发布清单", "release manifest fetched"},  /* UpdateOkFetched */
    {"没有找到《集合啦!动物森友会》", "Animal Crossing: New Horizons was not found"},  /* StateGameMissing */
    {"这个游戏版本暂不支持", "This game version is not supported yet"},  /* StateUnsupported */
    {"上次安装失败", "The last install failed"},  /* StateFailed */
    {"现在安装的 agent 不是当前发布版本", "The installed agent is not the current release"},  /* StateRepair */
    {"已安装的文件不完整", "The installed files are incomplete"},  /* StateIncomplete */
    {"还没有安装 agent", "The agent is not installed yet"},  /* StateNotInstalled */
    {"agent 已安装 · 有新版本", "The agent is installed - a newer version exists"},  /* StateUpdateAvailable */
    {"agent 已安装，可以使用", "The agent is installed and ready"},  /* StateInstalled */
    {"请先在这台机器上安装游戏，回来按一下重新检测", "Install the game on this console first, then check again"},  /* StateGameMissingSub */
    {"原因写在详情里", "The reason is in Details"},  /* StateUnsupportedSub */
    {"游戏 %s · agent %s", "Game %s - agent %s"},  /* HomeVersions */
    {"游戏 %s · 可安装 agent %s", "Game %s - agent %s ready to install"},  /* HomeVersionsAvailable */
    {"游戏 %s · agent %s → %s", "Game %s - agent %s -> %s"},  /* HomeVersionsUpdate */
    {"未安装", "not installed"},  /* ValueNotInstalled */
    {"安装 agent", "Install the agent"},  /* BtnInstall */
    {"更新到 %s", "Update to %s"},  /* BtnUpdate */
    {"已是最新", "Already up to date"},  /* BtnUpToDate */
    {"重新安装", "Install again"},  /* BtnRepair */
    {"重试", "Try again"},  /* BtnRetry */
    {"重新检测", "Check again"},  /* BtnRecheck */
    {"检查更新", "Check for updates"},  /* BtnCheckUpdate */
    {"卸载 agent", "Remove the agent"},  /* BtnUninstall */
    {"点一下即完成，之后重启游戏", "One tap, then restart the game"},  /* SubInstall */
    {"当前 %s · 最新 %s", "current %s - latest %s"},  /* SubRepair */
    {"再试一次，失败原因在详情里", "Tries once more; the reason is in Details"},  /* SubRetry */
    {"已经装好了，不需要操作", "Nothing to do"},  /* SubUpToDate */
    {"查看有没有更高版本的 agent", "Checks whether a newer agent exists"},  /* SubCheckUpdate */
    {"装好游戏后按一下即可", "Press after the game is installed"},  /* SubRecheck */
    {"删除已安装的文件", "Deletes the installed files"},  /* SubUninstall */
    {"动森增强 Mod(agent) 管理器 · 当前支持 %s", "Manager for the Animal Crossing agent - supports %s"},  /* HeadSubtitle */
    {"暂不可用", "Not available"},  /* BtnUnavailable */
    {"高级", "Advanced"},  /* SectionAdvanced */
    {"agent 版本", "Agent version"},  /* LabelEmbeddedAgent */
    {"安装记录", "Install record"},  /* LabelInstallRecord */
    {"日志", "Log file"},  /* LabelLogPath */
    {"%d 个文件，已逐个校验", "%d files, each one verified"},  /* ValueInstallRecord */
    {"没有安装记录", "no install record"},  /* ValueNoInstallRecord */
    {"详情", "Details"},  /* TabDetails */
    {"退出", "Exit"},  /* LabelExit */
    {"返回", "Back"},  /* LabelBack */
    {"相册(小程序)模式", "Album (applet) mode"},  /* AppletModeLibrary */
    {"装好后重启游戏即可使用聊天码", "Restart the game afterwards to use chat codes"},  /* ConfirmInstallNote */
    {"卸载后游戏恢复原样，随时可以再装", "The game goes back to normal; you can install again anytime"},  /* ConfirmUninstallNote */
    {"将安装 agent %s", "Installs agent %s"},  /* ConfirmWillInstall */
    {"开始安装", "Install now"},  /* ConfirmStart */
    {"删除文件", "Delete the files"},  /* ConfirmRemove */
    {"完成", "Done"},  /* Done */
    {"应用(全内存)模式", "Application (full memory) mode"},  /* AppletModeApplication */
    {"卸载成功", "Removal succeeded"},  /* ResultUninstallOk */
    {"卸载失败", "Removal failed"},  /* ResultUninstallFailed */
    {"将要删除的文件", "Files to remove"},  /* LabelPlanRemove */
    {"还有 %d 个文件 · 完整清单见详情页", "%d more files - see Details for the full list"},  /* LabelPlanMore */
    {"以下文件无法删除: %s", "could not delete: %s"},  /* UninstallFailed */
    {"重启游戏即可使用聊天码", "Restart the game to use chat codes"},  /* ResultReadyNote */
    {"管理器与存储卡失去联系:退出本程序后重新打开,再试一次", "Lost contact with the card: close the manager, open it again, then retry"},  /* ResultRelinkHint */
};

constexpr std::size_t kCount = sizeof(kStrings) / sizeof(kStrings[0]);

Language g_language = Language::ZhHans;

}  // namespace

const char *Text(StringId id, Language language) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kCount) {
        return "";
    }
    return language == Language::ZhHans ? kStrings[index].zh : kStrings[index].en;
}

unsigned StringCount() { return static_cast<unsigned>(kCount); }

std::string Format(StringId id, ...) {
    const char *format = Text(id, g_language);
    char stack_buffer[256];
    va_list args;
    va_start(args, id);
    const int needed = std::vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);
    if (needed < 0) {
        return format;
    }
    if (static_cast<std::size_t>(needed) < sizeof(stack_buffer)) {
        return stack_buffer;
    }
    std::vector<char> heap(static_cast<std::size_t>(needed) + 1);
    va_start(args, id);
    std::vsnprintf(heap.data(), heap.size(), format, args);
    va_end(args);
    return heap.data();
}

Language Current() { return g_language; }

void SetLanguage(Language language) { g_language = language; }

}  // namespace acnh_manager::i18n
