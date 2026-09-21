# 真机验收记录

验收在用户手工操作下进行(不自动部署、不自动上架)。每次验收后把结论写进本表:环境、安装了哪个
agent 版本、观察到什么、失败用例的原始现象。

## 基线环境(2026-09-17 实测)

| 项 | 值 |
|---|---|
| HOS | 22.1.0(`raw 0x00160100`) |
| 游戏 | ACNH 3.0.3,base `01006F8002326000`,update `01006F8002326800` |
| 标题版本 | 2228224(0x220000) |
| 更新包 Program 内容 id | `E10617820DB06889E1638499478DA0DE` |
| 运行中 main ModuleId | `FF1D1C05670DB6021C85B624A710B963` |
| `override_config.ini` | `[default_config] override_key=!L`(覆盖默认生效,按住 L 关闭) |
| exefs 现状 | `subsdk9` 105 837 B、`main.npdm` 1 652 B、`acnh-agent.version` 219 B |

## 验收矩阵

说明:标着 2026-09-17 的行是在**旧界面**上验的(当时是"状态 / 设置 / 安装确认 / 结果 / 卸载"
五页,开关都在"设置页");同样的行为如今落在"详情页(`R`)"与首页的 `X`(检查更新)、`Y`(卸载)
上。行内保留当时的说法,不做追改;2026-09-18 起的行按现在的界面记。

| 用例 | 手法 | 期望 | 结果 |
|---|---|---|---|
| 商店搜索安装 | 本地测试仓库(`repo.json`)或官方源 | 装到 `/switch/ACNH-Manager/acnh-manager.nro`,商店显示已安装 | 待执行 |
| 启动界面 | 相册启动 | 显示状态页:游戏版本/内容 id/覆盖提示/现有文件 | 已验(2026-09-17,截图核对;游戏未运行时也照常出图) |
| 环境识别 | 游戏关闭与运行两种状态各看一次 | 关闭时仍能显示版本与内容 id;运行时额外显示 ModuleId | 已验(关闭态);运行时态待执行 |
| 界面排版 | 状态页看"覆盖键提示 / 现有覆盖文件 / 取数异常"三处长文本 | 每行都落在卡片内:不压边框、不与相邻行重叠 | 已验(2026-09-17,像素级测量 + 截图) |
| 英文模式排版 | 设置页切语言后逐页看 | 最长标签 `Existing override files` 不压到值列 | 已验(2026-09-17,截图核对后切回中文) |
| 五页渲染 | 状态 / 设置 / 安装确认 / 结果 / 卸载各看一次 | 卡片高度随内容,文字不越界 | 已验(2026-09-17,逐页截图) |
| 退出 | 界面按 B(或 +) | 干净返回,不崩溃 | 已验(2026-09-17,日志有 `ui: loop exited` + `app: exiting`) |
| 干跑安装 | 设置页保持"干跑开启",按 A 安装 | 只校验并回报 files 数,dry_run=yes,不写盘 | 已验(2026-09-17,内嵌通道走完 3/3,卡上文件未变) |
| 真正安装 | 关闭干跑,按 A | 写入三个文件并生成 `state.json`;重启游戏后进岛出现"聊天码可用"气泡,`/9C9` 生成物品 | 已验(2026-09-17,内置 agent 0.11.0 / commit 8541a0956c30:三文件哈希与清单一致、`state.json` 生成;冷启动进岛就绪气泡出现,行首 `/9C9` 提交生成物品) |
| 内嵌发布通道 | 状态页看清单来源;设置页看来源行 | 显示"已内置(NRO 自带)"、门控 `supported build v3_0_3 [install]`,无"取数异常" | 已验(2026-09-17,截图) |
| 重复安装 | 已是最新时再按一次 A | 判定为 `up-to-date`,不写盘 | 已验(2026-09-18:首页显示"已是最新"、主按钮不可用;连按两次 A 后卡片逐字节不变,日志 `collect: … plan=up-to-date`) |
| 文件损坏 | 装完后改动已装文件,再进安装页 | 判定 `repair`,重装后回到发布哈希 | 已验(2026-09-18:故障注入把三个文件各追加一行,`Plan()` 判 `repair`,安装后三文件回到 `c47d2b47… / 23eea4ed… / 3eb52d5d…`,记录与卡上文件一致) |
| 卸载 | 状态页按 Y → 确认 | 三个文件与 `state.json` 删除,目录清空 | 已验(2026-09-18:日志 `uninstall: ok=1 files=3`,`exefs` 目录消失、`state.json` 不存在、无残留;随后重新安装成功) |
| 改过的文件仍能卸载 | 装完后手改一个文件(74 042 B)再卸载 | 按记录路径删除成功(不校验哈希) | 已验(2026-09-18:手改的 `subsdk9` 一并删除,日志 `uninstall: ok=1 files=3`) |
| 版本不支持 | 开发用清单里改掉 contentId 或 version | 门控拒绝,给出具体原因,不写盘 | 待执行 |
| 非发布清单 | 保留开发构建的清单(默认不允许开发清单) | 拒绝并提示 buildFlags 不是发布形态 | 已验(2026-09-17,CLI 与 App 设置页) |
| 旧金手指共存 | 保留 `cheats/<build id>.txt` | 状态页给出冲突警告 | 待执行 |
| 联网检查(传输层) | 首页按 `X` | 有网络时真的能连上托管端;**不再需要 CA 文件**(TLS 校验按 2026-09-18 的决定关闭,见 `docs/architecture.md` §9) | 已验(2026-09-18:日志 `update check: 失败: HTTP 404` —— 服务器**有响应**就说明 DNS→TLS 握手→HTTP 请求整条链路都通了;以前永远停在"缺少 CA 文件",一次都没真正联网) |
| 联网检查(取到清单并比较版本) | 托管端 `main` 上有 `agent-manifest.json` 后按 `X` | 显示"清单可用, agent x.y.z",远端更高时首页出现新版提示;失败只影响提示,不影响安装 | 已验(2026-09-19:提交并推送后按 `X` → 详情页与日志 `update check: 清单可用，agent 0.11.0`;远端与内置同版本,所以首页不提示新版。此前一直是 404 / 缺少 CA,这是第一次真正取到清单) |
| 界面语言重启后保持 | 详情页切语言 → 退出 → 重开 | 重开后仍是所选语言 | 已验(2026-09-18:`settings.json` 落盘;重开日志 `settings: language=en`,切回中文后重开是 `language=zh-Hans`) |
| 已装文件被外部删除 | 装完后在应用之外删掉 `exefs` → 重启 | 不再显示"已安装",而是如实报文件不完整并给重装按钮 | 已验(2026-09-18:状态行 `已安装的文件不完整`,日志 `collect: the card does not match the record: installed file … is not on the card`,`plan=repair`;按重装恢复成 `up-to-date`) |
| 检查更新时不冻结界面 | 首页按 `X`,趁检查还在进行时按 `R` 切页 | 页面立刻切走,检查仍在跑(按钮上是 spinner + "正在检查…") | 已验(2026-09-19:按 `X` 后 ~1 s 内截图仍是首页的 spinner,再按 `R` 立刻到详情页且"联网检查"行写着"正在检查…";本轮那次请求最终 8 s 超时,整个窗口界面都可用。改之前是同步调用,这几秒整机 UI 无响应) |
| 工作线程能起来 | 启动后看日志 | 日志有 `update check: started`;起不来时必须报错而不是静默 | 已验(2026-09-19:修复前 `threadCreate rc=0x00001759`(`LibnxError_BadInput`,栈没页对齐),表现为按 `X` 毫无反应;改为 `alignas(0x1000)` 后有 `started` 与结果行,并且"线程起不来"现在也按失败上报) |
| 启动静默检查 | 启动应用后不碰任何键 | 有新版时首页自己切到"更新到 X";失败静默(只进日志与详情页) | 已验(2026-09-19:一次启动的日志里 `update check: started` 紧跟在 `ui: environment collected` 之后,首帧照常画出;托管端有更高版本时首页直接显示"更新到 0.11.1",无网络时副标题保持平常措辞,原因在详情页) |
| 清单签名:篡改被拒 | 改掉托管端的清单内容但用旧签名 | 报"检查失败: 清单签名校验失败",不显示版本、不下载 | 已验(2026-09-19:把镜像上的 `agent.version` 改成 `0.11.9` 不重签 → 副标题"检查失败: 清单签名校验失败",日志 `update check: signature does not match rc=-0x4E00`;换回签名文件后同一请求变成 `manifest ok, agent 0.11.0`) |
| 清单签名:缺签名文件 | 只放清单、不放 `.sig` | 报"检查失败: 清单签名校验失败" | 已验(2026-09-19:下载路径会取 `<清单URL>.sig`,文件名不对时日志是 `update check: no signature (HTTP 404)`;这条抓出了产物名写成 `agent-manifest.sig` 的错,现统一为 `agent-manifest.json.sig`) |
| 一键更新 | 托管端发布更高版本后按 `X` → `A` → `A` | 首页"更新到 X"→确认页列出三个文件→下载、按清单校验、安装成功 | 已验(2026-09-19:镜像上发 `0.11.1`(内容仍是 0.11.0 的三个文件)→ 首页"agent 已安装 · 有新版本 / 0.11.0 → 0.11.1"、主按钮"更新到 0.11.1"→确认页→`install: ok=1 files=3`,结果页"安装成功",首页回到"已是最新") |
| 装的是哪一版 | 让卡上的版本与 App 自带的版本不同,再看首页/详情 | 显示**卡上**的版本;两者不同时详情在同一行说明 | 已验(2026-09-19:卡上 0.11.1、App 自带 0.11.0 时,首页写 `agent 0.11.1`,详情页写 `agent 版本 0.11.1(随应用自带 0.11.0)`;此前显示的是 App 自带的版本,更新后会让人以为没更新成功) |
| 未安装时的检查结果 | 卡上没有任何 agent 时按 `X` | 副标题如实说发布端有什么,不说"已是最新" | 已验(2026-09-19:显示"发布版本 0.11.0",与首页"可安装 agent 0.11.0"一致;此前会写"已是最新 0.11.0",而那时并没有任何"本机版本") |

## 状态与设置(2026-09-18)

两件都在真机上按上面的矩阵验过,补充两点实现事实:

- `Plan()` 只比较**安装记录与清单**,不看卡;卡上是否真有那些文件由
  `install::VerifyInstalledFiles()` 在每次 `Collect()` 之后核对(存在 + sha256 一致),不一致即报
  `HomeKind::Incomplete`。它只在计划本来是 `UpToDate` 时生效,不会覆盖"该装 / 该修"的判定。
- 界面语言存 `/switch/ACNH-Manager/settings.json`(与 `state.json` 分开,因为卸载会删掉后者)。
  文件缺失或损坏都退回默认语言,并把原因写进 `log.txt`,不挡启动。

测试提醒:用 sys-agent 的 FTP 给目录**改名**时,同一路径可能又出现(实测 `RNTO` 之后 `exefs`
与 `exefs-hidden` 同时存在),所以模拟"文件消失"请用**删除**(先删文件再 `RMD` 目录)。

## 崩溃记录(2026-09-17)

`/atmosphere/crash_reports/` 里只有 12:18–12:26 的四份报告,进程都是 `hbloader`
(`010000000000100d`),死因是写越界(Data Abort),对应"按 + / 进游戏就整机重启"。
根因是绘制高度用了 `height_aligned`(768 行)而线性影子缓冲只有窗口高度(720 行),
每帧越界写堆 48 行、约 245 KB,破坏与加载器共享的进程堆(见 `docs/architecture.md` §7)。
改成按 `win->height` 绘制后,同一台机器上反复启动、翻页、切语言、退出都没有再产生新报告;
历史日志里 04:47 与 04:51 两次构建的运行都以 `app: exiting` 正常收尾。
注意:从 Home 挂起再重新启动时,applet 是被外部结束的,日志会停在
`ui: init ok` 而没有退出行 —— 这不等于崩溃,要和"收尾缺行 + 有新崩溃报告"区分开。

**崩溃证据从哪来、要不要留**:现场在卡上 —— `/atmosphere/crash_reports/*.log`(Atmosphère
的文本报告:进程名/Program ID、异常类型与地址、栈回溯)和 `/atmosphere/erpt_reports/*.bin`
(同一批错误的 ERPT 二进制记录),排查时用 sys-agent 的 FTP 取回即可,不必留副本。我们撞到的
那一份长这样:`Process Name: hbloader`、`Program ID: 010000000000100d`、`Type: Data Abort`、
`Result: 0x4A8 (2168-0002)`,出错地址远离我们自己的缓冲。看到"hbloader + Data Abort"的报告,
先查上面这两条(绘制高度 clamp、退出模式),再怀疑卡或系统服务。

## 装到游戏目录的实测结论(2026-09-17)

- **applet 模式可以写 SD 卡,包括 `/atmosphere/contents/<tid>/exefs/`**:写盘探针在游戏目录里
  完成 create→SetSize→write→flush→read→delete 全部返回 0;不需要为了写盘去要 application 模式。
- **`fs*` 只接受绝对路径**:同一探针里,`switch/...` 与 `atmosphere/...` 两种相对写法都在
  `create` 上返回 `0x002EEA02`。这与"干跑 3/3 通过、真装第一步就失败"的现象完全对应,
  细节见 `docs/architecture.md` §4.1。
- 装完后 `exefs/` 三个文件的 sha256 与发布清单逐字一致(`subsdk9` 74 062 B、
  `main.npdm` 1 652 B、`acnh-agent.version` 219 B),`/switch/ACNH-Manager/state.json` 记录了
  agent 版本/commit/contentId/buildId 与三个文件的 size+sha256,可用于卸载回滚。

## 触摸输入实测(2026-09-17)

用 `/switch/ACNH-Manager/dev-touchprobe` 开关在真机上验证(hid 共享内存 + 触点坐标):

```
touch probe: hid shared memory ready
touch probe: down at x=1217 y=51 count=1 diameter=67x89
touch probe: released at x=1213 y=56
```

- **applet 模式可以读触摸**:`hidInitializeTouchScreen()` 成功、共享内存就绪(与 Sphaira 的用法一致);
  触摸因此在设置里是"能用就用、不能用就退回按键",不是必需能力。
- **`HidTouchState.x/y` 就是 1280×720 屏幕坐标**:手指点在右上角,记录值与位置吻合,无需面板坐标换算。
- 指腹接触面约 `67×89` px → 可点目标做到 100 px 以上、间距 ≥ 20 px 才不至于误触。
- 注意:`hidInitializeTouchScreen()` 在失败时会 **abort 整个进程**(libnx 行为),所以它只在这条
  探针路径与 `ui::Touch::Init()` 里各调用一次,其余地方先查共享内存再读状态。

## 退出路径:相册黑屏与修复(2026-09-17)

现象与定位(同一相册会话,同一份 NRO,只有"是否走 applet 退出命令"这一个变量):

| 构建 | 第 1 次退出 | 第 2 次退出 | 第 3 次退出 |
|---|---|---|---|
| `__nx_applet_exit_mode = 0`(返回加载器菜单) | 正常 | 正常 | **相册黑屏**,进程仍在,按 Home 后黑屏/白屏 |
| `__nx_applet_exit_mode = 1`(终止 applet,回主界面) | 正常 | 正常 | 正常 |

- 黑屏时应用侧日志完整(`ui: loop exited` → `app: exiting`),`sysagent system process-list`
  里 `010000000000100D`(hbl/相册)仍在,`capssc` 截屏全黑 → 不是崩溃,是显示资源耗尽。
- 与触摸无关:同一份二进制加/不加触摸初始化都能正常退出,黑屏只跟"同一相册会话里第几次
  启动/退出"有关。机制与取舍见 `docs/architecture.md` §10。
- 修复后的验收(2026-09-17 23:2x,`src:2bfd8b92` + exit mode 修复,`acnh-manager.nro`
  sha256 `5af1f9ce…`):连续 6 轮"Home → 相册 → Sphaira → ACNH-Manager → B 退出",每轮都用
  日志确认应用真的跑起来画过帧(`ui: first frame done`),每轮退出都干净回到主界面,无黑屏、
  无崩溃报告;对比基线(修复前)在第 3 次退出即黑屏。

## 触摸点击失效与修复(2026-09-18)

真机复现:首页"重新安装"按钮用手指点击**毫无反应**(截屏前后差异 0.0%),但 `+`/`B`/方向键
一切正常——也就是触摸被读到、却没有触发动作。

原因在按下的"松开帧":`hidGetTouchScreenStates` **只在手指接触时给出坐标**,松开那一帧
`state.count == 0`,`ui::Touch::Poll()` 于是返回 false 且不写 x/y,而调用方拿这次(全 0 的)
坐标去和按下点比对距离,于是每次点击都被当成"拖出屏幕一大截"的滑动而丢弃。

修复:把判定收进 `source/ui/action.hpp` 的 `TapTracker`——按下时记住动作与落点,手指在
按下期间持续记录最后坐标与最大偏移,松手时要求"全程偏移 ≤ 32 px"且"抬手点仍落在同一个
启用动作上"才触发;`tests/host_tests.cpp` 用 5 组用例把这条规则钉住(含"拖出去再拖回来
不触发")。

真机验收(2026-09-18,`acnh-manager.nro` sha256 `d8550268…`):

| 手势 | 屏幕变化 | 结果 |
|---|---|---|
| 点击首页"重新安装"(640,341) | 30.0% | 进入"安装确认"页(修复前同一手势 0.0%) |
| 从该按钮拖到 (300,650) 后松手 | 0.0% | 不触发任何动作(滑动不会误触按钮) |

## 安装的原子性与故障注入(2026-09-18)

起因:连续"安装 → 卸载 → 安装"时,第二次安装死在 `backup …/main.npdm rc=0x0000D401`,并且
**把已装好的目录改成了半成品**(只剩 `subsdk9`,另外两个文件不见了)。

修复(`source/install/engine.cpp`):安装改成两阶段——`PrepareFile` 只写 `<目标>.acnh-tmp` 并回读
校验(不碰已装文件),全部就绪后 `CommitFile` 才逐个"备份 `.acnh-old` → rename 落位";提交阶段
任一步失败即 `RollbackFile` 恢复已落位文件并清掉临时文件。备份/落位按**文件系统实际状态**判定,
不再靠错误码(`0xD401` 的语义见 `docs/architecture.md` §2.1)。规则与代码细节见
`docs/architecture.md` §8.1。

### 故障注入用例

手法(`tools/device-tests.py all`,在真机上跑,每步都自证):

- 先让首页的安装按钮**真的可用**:改写 `state.json` 里一个文件的 sha256,`Plan()` 于是判成
  `repair`,而卡上三个已装文件保持原样 —— 于是回滚有真东西可恢复;
- 再把三个已装文件各追加一行标记(大小 +56 B):这样"回滚"与"重新写入同一份发布文件"在卡上
  不再长得一样,断言"逐字节一致"才真正等于"旧文件回来了";
- 每一步都先截屏判定所在页面、再按键;并用 `log.txt` 的 `collect: gate=… plan=…` 行证明按下去
  的确实是可用的安装按钮;按完对卡片逐文件取哈希。

| 用例 | 注入 | 期望 | 实测(`65bf4c76…`;同一整轮在最终构建 `37fd216f…` 上复跑同样全绿) |
|---|---|---|---|
| T1a 可清理的残留 | `exefs/main.npdm.acnh-tmp` 放**空目录** | 启动即清掉,安装照常完成 | PASS:日志 `cleanup: removed 1 leftover file(s): main.npdm.acnh-tmp`;安装后三文件回到发布哈希 `c47d2b47… / 23eea4ed… / 3eb52d5d…`,记录与卡上文件一致 |
| T1b 清不掉的残留 | 同一路径放**非空目录** | 拒绝安装、卡片逐字节不变 | PASS:日志 `cleanup: removed 0 leftover file(s): main.npdm.acnh-tmp (could not remove)`;结果页"写入失败: create …/main.npdm.acnh-tmp rc=0x00000402";三文件、`state.json`、目录列表与注入前完全一致,无新增残留 |
| T2 记录写不进去 | `/switch/ACNH-Manager/state.json.acnh-tmp` 放非空目录(三个游戏文件仍可写) | 三个游戏文件回滚、卡片逐字节不变 | PASS(修复后):结果页"写入 state.json 失败: create …/state.json.acnh-tmp rc=0x00000402";三个被手改过的文件(74 098 / 1 764 / 331 B)在失败后回到注入前的字节,目录无 `.acnh-old` 残留 |
| 恢复 | 清掉全部注入 | 正常安装 | PASS:三文件写回发布哈希,记录与卡上文件一致 |

**T2 第一次跑就炸出一个真 bug**:修复前,安装把三个文件都落位、写 `state.json` 失败、然后
"回滚" —— 但回滚用的 rename 落在**已存在**的目标路径上(FAT 不允许),于是 rename 静默失败:
游戏目录里留下三个 `.acnh-old` 备份、新文件留在原位,而 App 却报"安装失败";记录与文件从此
对不上。修复:`RestoreBackup()` 先删目标再改回备份,且只有确认备份真的在才动目标;`CommitFile`
里同类的恢复路径一并改用它。这就是"回滚看起来实现了、其实没生效"被注入用例逼出来的过程。

### 路径贴着映射边界:`0xD401` 的真根因(2026-09-18,已修复)

现象:某些序列之后,**同一个应用会话里的后续安装 / 卸载一律失败**,错误码都是 `0xD401`,而且每次
失败的步骤都不一样(`mkdir …/01006F8002326000` → `create …/subsdk9.acnh-tmp` →
`open …/subsdk9.acnh-tmp` → `rename into …/subsdk9` 都出现过);同样这几步在**新开的会话里必然成功**,
酷似"这个进程跟存储卡失联了"。最早是 T1a 用例里单独撞见一次(失败原因当时只在屏幕上、日志里
没有,查不到现场),因此先给 `Install()`/`Uninstall()` 补了 `install: ok=… error=…` 日志行,再按
日志把复现步骤定下来:

1. 退出应用 → 重新打开(新的应用会话);
2. 在应用运行期间用 FTP(sys-agent 6001)往卡上写一次这个会话没写过的文件(测试里给
   `exefs/subsdk9` 追加一行);
3. 在应用里卸载(`uninstall: ok=1`);
4. 紧接着安装 → `install: ok=0 … rc=0x0000D401`。

| 排查 | 结果 |
|---|---|
| 目录是否真的没了 | 没有:FTP 能列出 `atmosphere/contents/01006F8002326000`,卡上文件健康 |
| 同一会话内重开 mount(`fsFsClose` + `fsOpenSdCardFileSystem`)后重试 | 无效(仍 `0xD401`,只是失败点后移) |
| 同一会话内重建 fs 会话(`fsExit` + `fsInitialize`)后重试 | 无效 |
| 同一会话内连续重试 4 次 | 全部失败,失败点在不同文件之间漂移 |
| 退出应用、重新打开后再装 | **成功**(启动清理顺手清掉失败留下的 `.acnh-tmp`),记录与卡上文件一致 |
| 去掉第 2 步(不写卡)再走同一序列 | 成功 |

**根因(2026-09-18 定位)**:与卡、与"会话失联"都无关,是**路径字符串在内存里的位置**。
`fs*` 把路径当作声明长度 `FS_MAX_PATH`(0x301)的 IPC 缓冲传出去,服务端按这个长度映射一整段
页对齐窗口;路径落在某个映射末尾 0x301 字节内时,窗口就伸进未映射页,内核返回
`InvalidMemoryState`。设备端逐调用追踪抓到的现场:

```text
fsop: mkdir rc=0x0000D401 ptr=0x329C697E80 region=0x329C674000+0x24000
fsop:   window=0x329C697E80..0x329C698181 region_end=0x329C698000 last_page type=0x0(unmapped)
fsop: mkdir rc=0x00000000 ptr=0x329D99D170 region=0x329D619000+0xF7E7000   # 同一个会话、同一个目录
fsop:   window=0x329D99D170..0x329D99D471 region_end=0x32ACE00000         # 窗口在映射内 → 成功
```

即:窗口越界的那次失败、窗口在界内的那次成功,都发生在同一个会话、同一个目录上,相隔毫秒。
哪个分配落在映射末尾由分配器决定,所以 `std::string` 拼出的路径时好时坏、失败点还会在文件之间
漂移,重启应用后换个位置又好了 —— 这也解释了"外部写卡 + 卸载"这条复现路径为什么时灵时不灵。

**修复**:所有交给 `fs*` 的路径先复制进 `util::FsPath`(`source/util/fs_path.hpp`,长度
`2 × FS_MAX_PATH` 的局部缓冲,窗口不可能越出);引擎、环境探测、主程序、日志与开发开关检查全部
改走它(后两处是 2026-09-20 补的,见文末"路径缓冲规则补完")。规则与实测细节见
`docs/architecture.md` 第 4.2 节。

按同样的证据向 libnx 提过报告(它的 `fsFs*` 把路径缓冲声明成固定 `FS_MAX_PATH`)。上游把那份报告
移到了**非公开仓库**,链接对普通账号不再可见,本仓库因此不再跟踪它的状态;后来从带符号的官方
SDK NSO 得到的结论是:官方客户端把路径拷进自己的 `FS_MAX_PATH` 零填充缓冲、装不下就返回 fs 的
`TooLongPath`(6003),再按完整声明长度发出去 —— 与这里的做法同形(见 `docs/architecture.md`
第 4.2 节)。

**验证**(`acnh-manager.nro` sha256 `a8860c0c…`):原先必失败的复现(全新会话 → 手改 `subsdk9` →
卸载 → 安装)连跑 4 次全部成功(`uninstall: ok=1 files=3` + `install: ok=1 files=3`,记录与卡上
文件一致),其中一次是在**没有** `dev-fsprobe` 开关的情况下跑的;四个故障注入用例全绿;修复前同一
脚本 6 次全部失败。

结果页仍保留一行兜底提示(`ResultRelinkHint`):万一再出现这个错误签名,「管理器与存储卡失去
联系:退出本程序后重新打开,再试一次」是实测有效的恢复动作。

## 启动配方(测试工具用,2026-09-18)

从桌面直达本应用的按键序列(括号内为该动作之后的等待),已在真机确认落在应用首页:

```
HOME(2s) → DD(1s) → DR(1s) → DR(1s) → DR(1s) → A(2s) → DD(1s) → DR(1s) → A(2s)
```

- `DD/DR` 是 sys-agent 的方向键名(小写 `Down`/`Right` 会被当成未知按键而**什么都不按**);
- 相册要几秒才加载完,间隔太短(0.5s/1s)时后面两下方向键会落在加载空档里而丢失;
- 该序列已固化在开发脚本的 `launch_app()` 里(过程材料,不入库)。

## 页眉/页脚按钮不可点与修复(2026-09-18)

现象:首页右上角的 `L 状态` / `R 详情` 是画出来的按钮,但**点击没有任何反应**——它们当时
只被键盘分支处理(`HandleKeys` 里的 L/R),没有登记进动作表 `m_actions`,所以触摸命中测试
永远找不到它们。顺带发现同一类问题:详情页是只读页,却不清理上一页的动作表,于是详情页上
"上一页按钮的位置"仍然可以点中(点空白处会跳去安装确认页)。

修复(2026-09-18,`acnh-manager.nro` sha256 `370470e1…`):

- 页眉两个标签页 + 页脚 `Ⓑ` 提示全部登记为动作,与 L / R / B 键**走同一条执行路径**
  (`ActivateAction`);绘制几何与命中矩形共用同一个布局函数(页眉在 `source/ui/header_tabs.hpp`,
  页脚在 `app.cpp` 的 `LayoutFooterHint`),不会再出现"画得到、点不到"。
- 动作表在每页自己的控件之后追加这些"外壳"控件,保证进入页面时焦点仍在主按钮上;
- 详情页开头清空动作表,只留下外壳控件(点空白处不再误触上一页的按钮);
- 主机测试新增 `TestHeaderTabs()`(748 checks 总数):两个标签页画在哪就能点哪、两个命中矩形
  永不重叠、目标不越出页眉。

真机验收(2026-09-18):

| 手势 | 屏幕变化 | 结果 |
|---|---|---|
| 点击 `R 详情`(1150,50) | 26.4% | 切到详情页(修复前同样点击 0.0%) |
| 点击 `L 状态`(1010,50) | 26.4% | 切回状态页,画面与起始帧完全一致(0.0%) |
| 按 `R` / `L` 实体键 | 26.4% / 26.4% | 键盘通路未受影响 |
| 详情页点击页脚 `Ⓑ`(55,698) | 26.4% | 返回状态页(仍在应用内) |
| 状态页点击页脚 `Ⓑ` | — | 退出应用,干净回到主界面 |

## 联网检查与一键更新(2026-09-19)

这一轮把"检查更新"从"只提示版本"变成"能下载并安装新版",同时把检查挪到工作线程上,
并加了一次启动静默检查。验收方式、证据与当时的网络状况如下。

**托管端**:今天这台机器的网络**连不上当时的发布托管端**,
所以正路线是用**本机 HTTPS 镜像**(自签证书、`agent-manifest.json` + `.sig` + 三个 payload 文件)
验的:一台 Mac 起静态服务,临时把 `kDefaultManifestUrl` 指向它,构建测试用 NRO 部署到真机;
验完把地址改回正式托管地址重新构建(`tools/verify-release.py --nro` 核对构建戳来自当前源码树)。
托管端上那条正路(取清单 → 验签 → 一键更新)因此**只验到了传输层**:同一份代码、同一个域名,
区别只在托管端连通性;换托管方后按同样的用例补验(见下面"托管端迁移到 Gitee"一节)。

一次成功检查的日志形状(镜像在 `https://<局域网 IP>:9443/`,清单 `0.11.1`):

```
update check: started (url=…/agent-manifest.json)     ← 工作线程起来了
update check: manifest ok, agent 0.11.1               ← 清单 + .sig 都取到、验签通过、解析通过
install: ok=1 files=3                                 ← 一键更新:三个文件按清单 size/sha256 校验后落盘
```

失败与边界:

| 情形 | 现象 |
|---|---|
| 托管端连不上 | 副标题"检查失败: 网络不可用",日志 `Couldn't connect to server`;启动那次静默检查不打扰用户 |
| 清单被改但没重签 | 副标题"检查失败: 清单签名校验失败",日志 `signature does not match rc=-0x4E00` |
| 只有清单没有 `.sig` | 日志 `no signature (HTTP 404)` |
| 工作线程建不起来 | 修复前:日志 `threadCreate rc=0x00001759`、按 `X` 毫无反应;现在按"检查失败: 应用内部错误"上报,详情页与日志带原始 `rc` |
| 检查期间操作 | 界面照常响应:spinner 在转、可切页、可退出 |

本轮改掉的两个真问题:

1. **`threadCreate` 要求栈页对齐**:`source/net/update_task.cpp` 里工作线程的栈原先只写了
   `alignas(16)`,libnx 直接返回 `LibnxError_BadInput`(`0x1759`,判定逻辑见
   `src/libnx/nx/source/kernel/thread.c`:"Verify alignment of provided memory")。
   现为 `alignas(0x1000)`。教训:这个错误只有真机才会暴露,主机侧测试与构建都不会发现。
2. **签名文件的名字**:App 取的是 `<清单 URL> + ".sig"`,发布脚本原先把产物写成
   `agent-manifest.sig`,两边对不上(线上表现就是"验签失败")。现统一为 `agent-manifest.json.sig`,
   工具、文档与镜像一致。

回归:改完跑 `tools/device-tests.py all`(t1a/t1b/t2/restore 四例全过)与
`tools/device-tests.py cycles --cycles 3`(三轮启动/退出都干净),外加 `make -C tests`(754 checks)。
本轮全部过程材料(镜像、证书、截图、日志)都在已忽略的 `build/scratch/` 下,不进仓库。

## 托管端迁移到 Gitee(2026-09-20)

**为什么迁**:2026-09-19 那轮验收暴露出当时的发布托管端在作者这条网络上不可用 —— 连续三次
443 直连全部失败(18 s / 12 s 级等待),而同一时刻 Gitee、指南站都是零点几秒返回。App 侧只会如实报"网络不可用"并退回内置版本,但"检查更新/一键更新"在作者机器上等于不可用,
所以整体换到 **Gitee**(两个仓库:`acnh-game/acnh-manager`、`acnh-game/acnh-agent`)。

**主机侧核对(2026-09-20,`curl` 匿名、不带任何凭据)**:

| 项 | 结果 |
|---|---|
| 仓库公开可访问 | `https://gitee.com/acnh-game/acnh-manager` → 200 |
| raw 取清单 | `…/raw/main/agent-manifest.json` → 302(跳 `raw.giteeusercontent.com` 带签名地址)→ 200,内容与仓库里那份**逐字节一致** |
| raw 取 payload | `…/raw/main/packaging/agent/0.11.0/subsdk9` → 200,73986 B,sha256 `c47d2b47…` == 清单里记录的哈希 |
| API 可用 | `gitee auth status` → `gitee.com / leolovenet`;`gitee api /repos/acnh-game/acnh-manager/releases` → `[]`(仓库可达,尚无发行版) |

**对外地址(迁移后)**:四个地址共用一个 raw 前缀
`https://gitee.com/acnh-game/acnh-manager/raw/main/` —— agent 三件套、`agent-manifest.json`、
`agent-manifest.json.sig`、以及 App 自身的
`packaging/nro/acnh-manager-<版本>.nro`(见 `docs/release-process.md` 第 6 节)。
Gitee CLI **不能给发行版传附件**,所以 NRO 不走发行版资产,改为仓库内文件 + raw 直链;
`gitee release create` 只用来建给人看的发行版页面。

**本轮改到的地方**:`source/net/update.hpp` 的 `kDefaultManifestUrl`;内嵌清单的 `baseUrl`
(`data/manifest.bin`、`packaging/agent/0.11.0/manifest.json`、根目录 `agent-manifest.json`
三份同步改,改完重新签名);`tools/{import-agent-release,make-dev-manifest,make-store-package}.py`
里生成的地址;商店条目 `packaging/listing.json` 的 `url`;发布链新增第 5 步(把 NRO 拷进
`packaging/nro/`);文档与 `AGENTS.md` 的托管章节(远端命令与发行版工具都换成 `gitee`)。

**真机侧(2026-09-20,Switch 上线后复验)**:新地址的 NRO 部署到机器并实跑,
**传输层到 Gitee 通了** —— 日志两行连续出现:

```
update check: started (url=https://gitee.com/acnh-game/acnh-manager/raw/main/agent-manifest.json)
update check: no signature (HTTP 404)
```

第一行说明工作线程起来了;第二行是"清单取到了(200,含 302 跳 `raw.giteeusercontent.com` 那一步),
但同址的 `.sig` 还没推上去",这正是预期的中间状态。界面表现也符合设计:启动静默检查失败时首页
保持平常措辞,手动按 `X` 后按钮副标题是"检查失败: 清单签名校验失败",详情页"联网检查"一行给出
原始诊断 `失败: no signature (HTTP 404)`。

同一份构建上的回归:`tools/device-tests.py all`(t1a/t1b/t2/restore 全部通过,失败 0)与
`cycles --cycles 3`(三轮启动/退出干净)均通过,说明换托管端没有碰到安装/退出路径。

**正路已验(2026-09-20,推送到 Gitee `main` 之后)**:线上清单与签名与仓库里逐字节一致、
`openssl dgst -verify data/agent_pubkey.bin` 通过之后,在真机上按 `X` 得到完整正路:

```
update check: started (url=https://gitee.com/acnh-game/acnh-manager/raw/main/agent-manifest.json)
update check: manifest ok, agent 0.11.0      ← 取到清单 + 验签通过 + 解析通过
```

界面:首页"agent 已安装,可以使用 / 游戏 3.0.3 · agent 0.11.0",检查按钮副标题"已是最新 0.11.0";
详情页"联网检查: 清单可用,agent 0.11.0"。也就是说"从托管端取清单 → 验签 → 与已装版本比较"这条
链路在真机上是通的,**不再依赖本地镜像**。"更高版本 → 一键更新"的真机验证用的是本机镜像那几轮
(见上面"更新路径的三处修复"),同一份代码路径、同一个清单格式。上面的历史行按当时的事实保留,
不做追改。

## 更新路径的三处修复(2026-09-20,真机验证)

迁移完成后做的一轮代码审查找出三处问题,都在真机上复现、修好、再验一遍。复现用的是本机
HTTPS 镜像(自签证书 + 用项目私钥签过的清单),这样不必往托管端推任何东西;临时清单与镜像
在验证后已删除,机器最后也还原成自带发布(`plan=up-to-date`)。

**① 新版清单不支持本机构建时,不能顶掉自带发布**

- 复现(修复前):镜像放一份"agent 0.11.1 + `games[]` 只有 4.0.0"的已签名清单,机器仍是
  ACNH 3.0.3 + 已装 0.11.0 → 首页变成"这个游戏版本暂不支持 / Not available",详情页写着
  `title supported but version 2686976 != detected 2228224 [blocked]`。也就是说:App 自带的
  0.11.0 明明支持这台机器,却被自己的更新检查判成"不支持"(若还没装 agent,用户会被彻底挡在
  安装之外)。
- 修复:采纳规则抽成纯函数 `net/update_policy.hpp::DecideAdoption()`(主机测试新增 32 条断言
  钉住:无可比版本 → 不采纳;同版本 → 不动;更新且门控接受 → 采纳;更新但门控拒绝 → 标记
  `unsupported`,保留自带发布);`App::ApplyUpdateCheck()` 调它,并新增文案
  "新版 %s 不支持当前游戏版本"。
- 复验(同机同镜像):首页回到"agent 已安装,可以使用 / 游戏 3.0.3 · agent 0.11.0",主按钮仍是
  "已是最新",检查按钮副标题变成"新版 0.11.1 不支持当前游戏版本"——既没有撒谎,也把原因说清了。

**② 安装期间要有进度页**(引擎同步跑在帧循环线程上,界面无法动画)

| 修复前 | 修复后 |
|---|---|
| 按 A 之后画面停在确认页,直到下载 + 写入全部结束(慢网可达数秒,读起来就是"卡死") | 新 `Page::Progress`:第一字节之前先画一帧,之后**每处理完一个文件再画一帧**;页面上没有可点的控件,也不处理按键,页眉标签与页脚 `B 返回` 一并撤掉 |

复验方式:镜像把每个 payload 请求延迟 1.5 s,一次装 3 个文件,时序截图依次是
`当前文件 subsdk9 (1/3)` → `main.npdm (2/3)`,整页只有
`正在安装 agent / 请不要关机,也不要退出应用 / 当前文件`,没有标签页、没有 `B` 提示。卸载走同一张
页面(标题换成"正在卸载")。`tools/device-tests.py` 的页面判定天然容纳这一页(非 `action` 页会
被当成中间态继续轮询),四例故障注入与三轮启停复跑全过。

**③ 结果页的失败原因不再被 URL 顶掉**:`network_source` 的下载错误改成
`download <文件名>: <原因>`(原来拼的是完整 URL,结果页那行限两行、超长走省略号,正好把真正
的原因截掉);URL 由清单的 `baseUrl + files[].source` 唯一确定,日志与详情页照旧可查。

**这一轮跑过的检查**:`make -C tests` 791 checks / 0 failures;`tools/check-i18n.py` 116 条同序;
`tools/verify-release.py --nro` 36 项(含清单签名);`py_compile`、`bash -n` 通过;真机
`device-tests all`(t1a/t1b/t2/restore)与 `cycles --cycles 3` 全过;NRO 里只剩 Gitee 地址。

## 版本 1.0.0 与发布自检补强(2026-09-20)

第二轮审查结论里的 P2-1 与 P3-1/2/4 已修,P3-3/5/6 按约定只留记录不改行为;同时把应用版本从
`0.1.0` 提到 **`1.0.0`**(应用从未公开发布过,所以没有需要迁移的旧版本)。

**发布自检补强(审查里那条"会静默炸掉所有人"的缺口)**

| 检查 | 抓住什么 |
|---|---|
| 锁里的 `signingPublicKeySha256` ↔ `data/agent_pubkey.bin` | 换钥匙:已经发出去的 App 只认它被编进去的那把,所以换身份必须在发布 diff 里显式出现 |
| NRO 的 `.rodata` 里必须含当前公钥的原始字节(178 B) | "换钥匙 + 重签名但**忘了重建 NRO**":以前这样的产物能通过全部本地自检,玩家侧则每个清单都被拒 |

两条都做了反向验证(把公钥换掉 / 把锁里的指纹改成全 0),对应检查确实报 FAIL,而不是永远绿。

**其余修复**:详情页"清单来源"改成按**实际会用的那份清单**取词(自带 / 从发布端下载(agent x.y.z) /
开发清单路径),不再在更新窗口里嘴上说"随应用自带";`verify-release.py` 的 `--data/--record`
传相对路径或仓库外路径不再崩(以前 `relative_to()` 直接抛异常,检查一条都没跑);
`AGENTS.md` 目录规划补登 `update_policy.hpp`,`docs/architecture.md` §7 的页面清单补上进度页并写明
它是唯一"故意不画控件"的页面。

**真机(2026-09-20 晚,1.0.0 构建)**:部署后首页"agent 已安装,可以使用 / 游戏 3.0.3 · agent 0.11.0",
详情页 `agent 版本 0.11.0`、`清单来源 随应用自带`、`检测结果 supported build v3_0_3 (3.0.3) [up-to-date]`;
`device-tests all`(t1a/t1b/t2/restore)与 `cycles --cycles 3` 全过;`verify-release --nro` 39 项、
`make -C tests` 796 checks、`check-i18n` 117 条同序。

> 过程记录:当时发现卡上 agent 不在了,查 `log-history.log` 看到最后一条动作是
> `uninstall: ok=1 files=3`——是**测试脚本连按**走完 Y→A 正常卸载流程留下的,不是程序自己删的
> (应用的卸载只由 Y 键 + 确认触发);重新安装后状态回到 `up-to-date`。

## 路径缓冲规则补完(2026-09-20)

起因两条:上游把那条 libnx 报告移到了非公开仓库(链接不再可见,本仓库不再跟踪);同时拿到官方
SDK 的反编译证据 —— 官方客户端把路径拷进自己的 `FS_MAX_PATH` 零填充缓冲、装不下返回 `TooLongPath`
(6003),再按完整声明长度发出去(见 `docs/architecture.md` 第 4.2 节)。于是把"交给 `fs*` 的缓冲
必须自己盖住 `FS_MAX_PATH` 窗口"这条规则逐处核对了一遍,补上还漏的三处:

| 位置 | 原来 | 现在 |
|---|---|---|
| `source/log.hpp` 的 `Log::Sink::path` | `char[128]` —— 按 0x301 发出去时窗口越出数组 0x281 字节,只因 `Log` 是 `main()` 的局部(大栈映射)才没复发 | `util::FsPath::kBufferSize`(2 × `FS_MAX_PATH`),并用 `static_assert` 钉住尺寸 |
| `source/log.hpp` 的 `Log::Open()` | 在复制进 sink **之前**先用调用者的裸指针调 delete/create/open | 先 `util::FsPath`,三次调用与 sink 里的副本都用它 |
| `source/ui/app.cpp` 的开发开关(`dev-pause`) | 直接传字符串字面量 | 同样过 `util::FsPath`(字面量本来安全,收口是为了让规则没有例外) |

`util::FsPath` 自己也加了 `static_assert(kBufferSize >= FS_MAX_PATH, …)`,防止以后有人把缓冲改小。
刻意**没有**动的两处:`probe.cpp` 继续把裸指针交给 `fs*`(它的目的就是演示未保护指针的行为);
也没有对齐官方的 `TooLongPath` 返回值 —— 本应用的路径都是几十字节的固定串,超长只可能来自手工
放进卡里的开发清单,静默截断不影响正确性。

**验证**(`acnh-manager.nro` sha256 `77ed67d8…`,构建戳 `2026-09-20T05:16:28Z+72b366d-dirty+src:bdc00ef9`):
`make -C tests` 791 checks / 0 failures;`tools/check-i18n.py` 116 条同序;`./tools/build.sh` 零警告;
`tools/verify-release.py --nro` 36 项(含清单签名);真机 `device-tests all`(t1a/t1b/t2/restore)与
`cycles --cycles 3` 全过 —— 日志路径每次运行都会走到,这两组用例正好覆盖它。

## 聊天码说明页与小程序码(2026-09-21)

界面新增一页:**任何页面按 `+` 打开"聊天码是什么"**,再按 `+`(或 `B`)回到进来时的那一页 ——
`+` 从原先的"退出"改成这个。`B` 仍然是退出(状态页)与返回(其它页)。

- 入口两处:`+` 键,以及页眉右侧新增的 `+ 说明` 入口(与头部标签共用 `LayoutHeaderTabs`,打开
  说明页时它自己高亮);
- 内容:标题 + 三行说明(聊天码是物品编号的十六进制写法、行首 `/9C9` 的输入方式与一行多个、
  编号从《森友物码册》小程序查)+ 小程序码(中间白区里写着“物码”两个字,与原码 logo 同位置);
- 二维码**不是图片**:`tools/make-qr.py` 把 miniapp 仓库里的 `resources/小程序码.jpg` 采样成
  模块矩阵(识别结果:37×37 模块 / version 5,pitch 20 px,与原始图差异仅 1.64%,即中间 logo 圆盘区域),
  写进 `data/qr_miniapp.bin`(189 B),界面用矩形填充逐格画 —— 主机侧不需要任何图片解码器;
- 真机(2026-09-21):构建并部署后按 `+` 打开该页,再按 `+` 返回,返回后的画面与进入前**逐像素一致**;
- **待补**:用手机实扫屏幕上这个码确认可扫(采样把 logo 圆盘区域涂白,和原始码被 logo 遮住的面积相同,
  理论上与原始码同样可解码;我没有解码器,只能由手机确认)。
