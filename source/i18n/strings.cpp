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
    {"旧金手指代码", "Legacy cheat code"},  /* LabelLegacyCheat */
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
    {"清单来源", "Manifest source"},  /* LabelManifestSource */
    {"安装成功", "Install succeeded"},  /* ResultSuccess */
    {"安装失败", "Install failed"},  /* ResultFailure */
    {"这个版本没有带安装包，请更新应用后重试", "This build has no installer bundled; update the app and try again"},  /* ResultNoManifest */
    {"联网检查", "Update check"},  /* LabelUpdateCheck */
    {"(未检查)", "(not checked)"},  /* ValueNotChecked */
    {"清单可用，agent %s", "manifest ok, agent %s"},  /* UpdateCheckOk */
    {"失败: %s", "failed: %s"},  /* UpdateCheckFailed */
    {"正在检查…", "Checking…"},  /* SubUpdateChecking */
    {"已是最新 %s", "up to date: %s"},  /* SubUpdateUpToDate */
    {"发现新版 %s", "new version %s"},  /* SubUpdateNewer */
    {"检查失败: %s", "check failed: %s"},  /* SubUpdateFailed */
    {"发布版本 %s", "published %s"},  /* SubUpdatePublished */
    /* A release host that moved on without this console's game build: nothing to install, and
       the reason is not "you are up to date". */
    {"新版 %s 不支持当前游戏版本", "new %s does not support this game version"},  /* SubUpdateNewerUnsupported */
    {"正在安装 agent", "Installing the agent"},  /* ProgressInstalling */
    {"正在卸载 agent", "Removing the agent"},  /* ProgressUninstalling */
    {"请不要关机,也不要退出应用", "Do not power off or leave the app"},  /* ProgressNote */
    {"当前文件", "Current file"},  /* LabelProgressFile */
    {"聊天码是什么?", "What is a chat code?"},  /* GuideTitle */
    {"游戏里能直接把物品放进背包的短码:物品编号的十六进制写法,例如 9C9。", "A short code that puts an item straight into your bag: the item's id written in hex, for example 9C9."},  /* GuideWhat */
    {"在游戏里打开聊天栏(按 R), 行首输入 /9c9 再发送, 则会在背包第一个槽位生成\"金矿\"", "Open the chat in game (press R) and send a line that starts with /9c9: the first pocket slot gets a \"gold nugget\"."},  /* GuideHow */
    {"一行一次可以写入多个聊天码, 用空格隔开, 最多 10 个, 例如: /9c9 9c6 9c4", "One line can carry several codes, separated by spaces, up to 10, for example: /9c9 9c6 9c4"},  /* GuideHowMore */
    {"编号从《森友物码册》小程序里查:搜到物品就能复制它的聊天码。", "Look the id up in the mini-app: find the item and copy its chat code."},  /* GuideWhere */
    {"微信扫一扫,打开《森友物码册》", "Scan with WeChat to open the mini-app"},  /* GuideScan */
    {"功能说明", "Guide"},  /* TabGuide */
    /* The brand mark from the mini-app's logo; it is an emblem, so it is not translated. */
    {"物码", "物码"},  /* GuideMark */
    {"网络不可用", "no network"},  /* UpdateShortOffline */
    {"服务器返回 %ld", "server returned %ld"},  /* UpdateShortHttp */
    {"清单签名校验失败", "manifest signature check failed"},  /* UpdateShortSignature */
    {"清单不可用", "manifest is unusable"},  /* UpdateShortManifest */
    {"应用内部错误", "internal error"},  /* UpdateShortInternal */
    {"随应用自带", "bundled with the app"},  /* ValueEmbeddedManifest */
    {"从发布端下载(agent %s)", "downloaded (agent %s)"},  /* ValueNetworkManifest */
    /* The version on the card, plus what this build of the app carries when the two differ. */
    {"%s(随应用自带 %s)", "%s (bundled %s)"},  /* ValueAgentCarried */
    {"%s(本应用支持 %s)", "%s (this build supports %s)"},  /* ValueGameVersionSupported */
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
    {"没有找到《集合啦!动物森友会》", "Animal Crossing: New Horizons was not found"},  /* StateGameMissing */
    {"不支持当前安装的游戏版本", "The installed game version is not supported"},  /* StateUnsupported */
    {"上次安装失败", "The last install failed"},  /* StateFailed */
    {"现在安装的 agent 不是当前发布版本", "The installed agent is not the current release"},  /* StateRepair */
    {"已安装的文件不完整", "The installed files are incomplete"},  /* StateIncomplete */
    {"还没有安装 agent", "The agent is not installed yet"},  /* StateNotInstalled */
    {"agent 已安装 · 有新版本", "The agent is installed - a newer version exists"},  /* StateUpdateAvailable */
    {"agent 已安装，可以使用", "The agent is installed and ready"},  /* StateInstalled */
    {"没有检测到游戏本体 · 可用本应用安装一份", "No base game found - this app can install one"},  /* StateGameMissingSub */
    {"本应用目前无法处理这个版本", "This build cannot handle the installed version"},  /* StateUnsupportedSub */
    {"游戏 %s · agent %s", "Game %s - agent %s"},  /* HomeVersions */
    {"游戏 %s · 本应用支持 %s", "Game %s - this build supports %s"},  /* HomeVersionsUnsupported */
    {"游戏 %s · 可安装 agent %s", "Game %s - agent %s ready to install"},  /* HomeVersionsAvailable */
    {"游戏 %s · agent %s → %s", "Game %s - agent %s -> %s"},  /* HomeVersionsUpdate */
    {"未安装", "not installed"},  /* ValueNotInstalled */
    {"安装 agent", "Install the agent"},  /* BtnInstall */
    {"更新到 %s", "Update to %s"},  /* BtnUpdate */
    {"已是最新", "Already up to date"},  /* BtnUpToDate */
    {"重新安装", "Install again"},  /* BtnRepair */
    {"重试", "Try again"},  /* BtnRetry */
    {"安装游戏", "Install the game"},  /* BtnInstallGame */
    {"检查更新", "Check for updates"},  /* BtnCheckUpdate */
    {"卸载 agent", "Remove the agent"},  /* BtnUninstall */
    {"点一下即完成，之后重启游戏", "One tap, then restart the game"},  /* SubInstall */
    {"当前 %s · 最新 %s", "current %s - latest %s"},  /* SubRepair */
    {"再试一次，失败原因在详情里", "Tries once more; the reason is in Details"},  /* SubRetry */
    {"已经装好了，不需要操作", "Nothing to do"},  /* SubUpToDate */
    {"查看有没有更高版本的 agent", "Checks whether a newer agent exists"},  /* SubCheckUpdate */
    {"装一份游戏本体", "Install a base game"},  /* SubInstallGame */
    {"删除已安装的文件", "Deletes the installed files"},  /* SubUninstall */
    {"动森增强 Mod(agent) 管理器", "Manager for the Animal Crossing agent"},  /* HeadSubtitle */
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
    {"检测到可能冲突的旧金手指代码", "Found an old cheat code that may conflict"},  /* HomeLegacyCheat */
    {"游戏安装/更新", "Game Install / Update"},  /* TabInstall */
    {"本机文件", "This console"},  /* InstallPickFile */
    {"从 SD 卡选一个文件安装", "Install a file from the SD card"},  /* InstallPickFileSub */
    {"下载安装", "Download and install"},  /* InstallDownload */
    {"填写网址,下载安装 —— 边下边装", "Paste an address; it installs as it downloads"},  /* InstallDownloadSub */
    {"上传安装", "Upload and install"},  /* InstallUpload */
    {"通过手机/电脑浏览器上传安装", "Upload from a phone or computer browser"},  /* InstallUploadSub */
    {"支持 nsp / nsz / xci / xcz", "nsp / nsz / xci / xcz"},  /* InstallFormats */
    {"正在等待连接……", "Waiting for a connection..."},  /* InstallWaiting */
    {"用手机或电脑的浏览器打开下面的地址", "Open this address in a phone or computer browser"},  /* InstallOpenAddress */
    {"在网页里粘贴下载网址 —— 不用一个字一个字地敲", "Paste the download address there - no need to type it out"},  /* InstallPagePasteHint */
    {"在网页里选择要安装的文件", "Pick the file to install there"},  /* InstallPagePickHint */
    {"上次没传完:再选一次同一个文件就能接着传", "The last upload stopped early - pick the same file again to carry on"},  /* InstallPageResumeHint */
    {"扫码打开", "Scan to open"},  /* InstallScanQr */
    {"网页服务没能启动,请改用「下载安装」或「本机文件」", "The page service did not start - use the download or SD-card entry"},  /* InstallNoPage */
    {"输入网址", "Enter address"},  /* InstallTypeUrl */
    {"连不上这个地址,检查地址与端口是否正确", "Cannot connect to this address; check the address and port"},  /* InstallNoNetwork */
    {"这个地址不完整或格式不对,请填写完整网址", "This address is incomplete or malformed; enter a full URL"},  /* InstallBadUrl */
    {"请先完全退出游戏(主页上把游戏关掉)再试一次", "Close the game completely first (HOME screen, then X), then try again"},  /* InstallGameRunning */
    {"浏览器停止发送了:重新打开页面,再选一次这个文件即可继续", "The browser stopped sending - re-open the page and pick this file again"},  /* InstallUploadStopped */
    {"这个地址不支持断点续传,请换一个地址或用手机上传", "This address cannot be resumed; use another address or upload the file"},  /* InstallRangeUnsupported */
    {"地址打不开(HTTP %s),检查一下地址是否写对", "The address answered HTTP %s; check that it is correct"},  /* InstallHttpStatus */
    {"用系统软键盘输入下载网址", "Type the download address on the system keyboard"},  /* InstallUrlHint */
    {"上次:%s", "Last: %s"},  /* InstallUrlLast */
    {"地址要支持断点续传(HTTP Range),不支持会被拒绝", "The address must support range requests, or it is refused"},  /* InstallUrlRangeNote */
    {"请先在系统设置中连接网络", "Connect to a network in system settings first"},  /* InstallNoNetworkTitle */
    {"连接到手机热点,或者与手机/电脑连接到同一个 Wi-Fi 下即可", "A phone hotspot, or the same Wi-Fi as your phone or computer, works"},  /* InstallNoNetworkHint */
    {"重新检测", "Check again"},  /* InstallRecheck */
    {"重试", "Try again"},  /* InstallRetry */
    {"版本", "Version"},  /* ConfirmVersion */
    {"类型", "Type"},  /* ConfirmType */
    {"大小", "Size"},  /* ConfirmSize */
    {"目标存储", "Install to"},  /* ConfirmTarget */
    {"装好后会自动安装 agent", "The agent is installed automatically afterwards"},  /* ConfirmAgentNote */
    {"本体", "Base game"},  /* TypeBase */
    {"更新", "Update"},  /* TypeUpdate */
    {"追加内容", "Add-on"},  /* TypeDlc */
    {"正在接收", "Receiving"},  /* ProgressReceive */
    {"正在写入", "Writing"},  /* ProgressWrite */
    {"正在收尾", "Finishing up"},  /* ProgressFinalize */
    {"等待浏览器继续传输… %d 秒, 超时后方可重新上传", "Waiting for the browser… %d s; you can upload again after the timeout"},  /* ProgressWaitBrowser */
    {"第 %d / %d 个内容", "Content %d of %d"},  /* ProgressContents */
    {"安装完成", "Installed"},  /* ResultInstalled */
    {"已安装,跳过", "Already installed - skipped"},  /* ResultSkippedInstalled */
    {"已有更高版本,已跳过", "A newer version is installed - skipped"},  /* ResultSkippedNewer */
    {"安装失败", "Install failed"},  /* ResultFailed */
    {"可以安装 agent 了(首页按 A)", "The agent can be installed now (press A on the home screen)"},  /* ResultAgentReady */
    {"--.--", "--.--"},  /* StorageUnavailable */
    {"microSD", "microSD"},  /* StorageSdCard */
    {"系统", "System"},  /* StorageSystem */
    {"安装游戏更新", "Install the game update"},  /* HomeInstallUpdate */
    {"升级完会自动安装 agent", "The agent is installed automatically once it is updated"},  /* HomeInstallUpdateSub */
    {"上一级", "Up one level"},  /* InstallUpOneLevel */
    {"目录", "Directory"},  /* InstallDirectory */
    {"安装包", "Package"},  /* InstallPackage */
    {"忽略", "Ignored"},  /* InstallIgnored */
    {"这个目录里没有可安装的文件", "No installable file in this directory"},  /* InstallNoFiles */
    {"这个文件不是可安装的安装包(nsp / nsz / xci / xcz)", "This file is not an install package (nsp / nsz / xci / xcz)"},  /* InstallNotAPackage */
    {"这个包里没有可安装的内容", "This package has no installable content"},  /* InstallNoContent */
    {"agent 不支持此版本(游戏/更新): 支持版本为: %s", "The agent does not support this version (game/update) - supported versions: %s"},  /* InstallAgentUnsupported */
    {"已写入 %s · %d 个内容 · %s", "Written to %s - %d contents, %s"},  /* ResultWroteLine */
    {"机器上已经有这个版本,没有改动", "This version is already installed; nothing changed"},  /* ResultSameVersionDetail */
    {"机器上已经有更新的版本(%s),没有改动", "A newer version (%s) is already installed; nothing changed"},  /* ResultNewerVersionDetail */

    {"传输进行中,请不要离开本页面", "Transfer in progress - please stay on this screen"},  /* ProgressDoNotInterrupt */
    {"机器上已经有更新的版本,没有改动", "A newer version is already installed; nothing changed"},  /* ResultNewerVersionPlain */
    {"agent 已经装好了", "The agent is already installed"},  /* ResultAgentAlreadyInstalled */
    {"L / R 切换页面", "L / R switch pages"},  /* FooterPagesHint */

    /* The step that follows a game install (the player asked for the game, the agent is what makes
       it useful): what it wrote, and the one case where it wrote nothing. */
    {"agent 已一并安装", "The agent was installed as well"},  /* ResultAgentFollowUpInstalled */
    {"agent 这次没能装上:回首页可以重新安装", "The agent was not installed this time - install it again from the home screen"},  /* ResultAgentFollowUpFailed */
    /* "<type> xN": one piece of a dump that carries more than one meta ("更新 ×1 · 追加内容 ×2").
       The word order works in both languages because the count follows the type. */
    {"%s ×%d", "%s x%d"},  /* MetaCount */
    /* Short names for the "what comes along" summary on the confirm card: the row's own values
       stay spelled out ("Base game", "Add-on"), but the summary shares its line with the type and
       has to stay clear of the cover -- the English long forms push it under the picture. */
    {"本体", "Base"},  /* MetaTypeBaseShort */
    {"更新", "Update"},  /* MetaTypeUpdateShort */
    {"追加内容", "DLC"},  /* MetaTypeDlcShort */
    /* The install page's header: this label sits to the left of the two storage blocks and says
       what the pair means (they are the control that picks the target storage). */
    {"安装到:", "Install to:"},  /* InstallToLabel */
    /* The footer note on the install page.  The tabs are hidden there, so the "L / R switch
       pages" note has no meaning and this one takes its place. */
    {"L / R 切换存储", "L / R switch storage"},  /* FooterInstallTargetHint */
    /* The install an interruption left behind (design section 10): the install page opens on it, and
       the details page lists it, so the space its placeholders hold is never invisible. */
    {"未完成的安装", "Unfinished install"},  /* PendingTitle */
    {"继续", "Continue"},  /* PendingResume */
    {"放弃并清理", "Give up and clean up"},  /* PendingAbandon */
    {"选择新的安装内容", "Pick something else"},  /* PendingPickNew */
    {"这条任务不能继续(来源是浏览器上传),可以放弃并清理", "This one cannot be continued here (it came from a browser upload) - give up and clean up instead"},  /* PendingUnresumable */
    {"继续:在浏览器页面里再选一次这个文件", "Continue: pick this file again in the browser page"},  /* PendingUploadResume */
    {"占位已被系统清理(重启会清空占位):继续会从头开始", "The placeholders are gone (a reboot clears them): continuing starts over"},  /* PendingGone */
    {"已接收 %s / %s", "%s of %s received"},  /* PendingProgress */
    {"占位 %s", "%s reserved"},  /* PendingReserved */
    /* On the confirm card, when this run is picking up where an interrupted one stopped. */
    {"继续上次未完成的安装(已接收 %s)", "Continuing the unfinished install (%s received)"},  /* ConfirmResumeNote */
    /* The transfer failed but the work is not lost: the task file and its placeholders stay. */
    {"未完成的安装已保留,下次进安装页可以继续", "The unfinished install was kept - continue it from the install page"},  /* ResultTaskKept */
    /* The browser page the console serves: the phone/computer half of the installer. */
    {"第 1 步 · 选择安装文件", "Step 1 - pick the file to install"},  /* PageStep1 */
    {"选择文件", "Choose a file"},  /* PageChooseFile */
    {"或把文件拖到这里", "or drop it here"},  /* PageDropHint */
    {"传输时请让本页留在前台;锁屏或切后台会中断,重新打开本页可从断点继续。", "Keep this page in the foreground while it transfers; locking the screen or switching away interrupts it, and re-opening this page continues from where it stopped."},  /* PageKeepAwake */
    {"或者 · 粘贴下载网址(由 Switch 主机下载)", "or - paste a download address (the console downloads it)"},  /* PageUrlTitle */
    {"https://example.com/xxx.nsp", "https://example.com/xxx.nsp"},  /* PageUrlPlaceholder */
    {"下载并安装", "Download and install"},  /* PageUrlButton */
    {"已交给 Switch 主机,请看它的屏幕", "The console has it - watch its screen"},  /* PageUrlSent */
    {"Switch 主机没有接受这个地址", "The console did not accept that address"},  /* PageUrlFailed */
    {"正在上传", "Uploading"},  /* PageStageUpload */
    {"Switch 主机已离线(可能已经离开安装界面)", "The console is offline (it may have left the install screen)"},  /* PageOffline */
    {"本页由 Switch 主机直接提供,「上传安装」全程不需要互联网。", "The console serves this page itself; uploading needs no internet connection."},  /* PageNoteNoInternet */
    {"Switch 主机屏幕上会同步显示同一份进度。", "The same progress is shown on the console's own screen."},  /* PageNoteHostScreen */
    {"回到 Switch 主机即可安装 agent", "Back on the console, the agent can be installed"},  /* PageAgentReady */
    {"%d 个内容 · %s → %s", "%d contents · %s → %s"},  /* PageWrittenLine */
    /* The browser's own upload loop (the page's JavaScript). */
    {"正在把文件交给 Switch 主机……", "Handing the file to the console…"},  /* PageUploadStarting */
    {"Switch 主机正在处理,请不要关闭本页……", "The console is working - keep this page open…"},  /* PageUploadWaiting */
    {"已发送 %d%%", "%d%% sent"},  /* PageUploadProgress */
    {"Switch 主机上已经有别的安装在进行了", "The console is already installing something else"},  /* PageUploadBusy */
    {"本页被切到后台了:浏览器会暂停这个页面,回到本页后重新选一次这个文件就能接着传", "This page went to the background: the browser pauses it - come back to this page and pick the file again to carry on"},  /* PageHidden */
    {"复制日志", "Copy log"},  /* PageCopyLog */
    {"清空日志", "Clear log"},  /* PageClearLog */
    {"已复制", "Copied"},  /* PageCopied */
    {"复制失败,请手动选中", "Copy failed - select it by hand"},  /* PageCopyFailed */
    {"已有另一个页面在给 Switch 主机供文件:请关掉那个标签页,再在这里选择文件", "Another page is already feeding the console - close that tab, then pick the file here"},  /* PagePeerActive */
    {"切换存储会重新读取安装包…", "Switching storage re-reads the package…"},  /* PageRereadOnSwitch */
    {"不可用", "unavailable"},  /* PageStorageUnavailable */
    {"等待选择安装文件", "Waiting for a file"},  /* PageWaitingPick */
    {"已就绪", "Ready"},  /* PageWaitingStart */
    {"已交给 Switch 主机,请稍候…", "The console has it - one moment…"},  /* PageStarting */
    {"Switch 主机现在不能切换存储(可能已经在安装)", "The console cannot change storage right now (an install may be running)"},  /* PageTargetRefused */
    {"Switch 主机没有接受这次开始,刷新页面再试", "The console did not accept this start - reload the page and try again"},  /* PageStartRefused */
    {"本次安装非当前页面发起,请点「返回」重新选择", "This install was started from another page - press Back and pick again"},  /* PageReselectToStart */
    {"切换存储会重新开始这次上传", "Switching storage restarts this upload"},  /* PageRestartUpload */
    {"请重新选择文件", "Pick the file again"},  /* PageUploadDropped */
    {"Switch 主机没有接受这次返回,刷新页面再试", "The console did not accept this back step - reload the page and try again"},  /* PageBackRefused */
    {"正在读取安装包信息…", "Reading the package information…"},  /* PageUploadInfo */
    {"调试日志", "Debug log"},  /* PageDebugTitle */
    {"最小化", "Minimize"},  /* PageDebugMinimize */
    {"最大化", "Maximize"},  /* PageDebugMaximize */
    {"还原", "Restore"},  /* PageDebugRestore */
    {"最新", "Latest"},  /* PageDebugLatest */
    {"传输中断了", "The transfer was interrupted"},  /* PageUploadFailed */
    /* The progress card's speed and time-left notes (both devices print the same sentence). */
    {"约 %s/s", "about %s/s"},  /* ProgressRate */
    {"剩余约 %d 秒", "about %d s left"},  /* ProgressEtaSeconds */
    {"剩余约 %d 分钟", "about %d min left"},  /* ProgressEtaMinutes */
    {"剩余约 %s 小时", "about %s h left"},  /* ProgressEtaHours */
    {"剩余约 %d 小时", "about %d h left"},  /* ProgressEtaHoursOnly */
    {"Switch 主机还在等待上一次上传;结束后才能选择新文件", "The console is still waiting for the previous upload; picking is disabled until it ends"},  /* PageUploadActive */
    {"Switch 主机在等待原连接继续传输… %d 秒, 超时后方可重新上传", "The console is waiting for the original connection… %d s; you can upload again after the timeout"},  /* PageWaitOriginal */
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
