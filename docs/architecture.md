# 架构

## 1. 职责

ACNH-Manager 是 `acnh-agent` 的安装器:识别控制台上游戏的实际构建,把对应 payload 写入 SD 卡,
并给出安装状态、失败原因与卸载回滚。它不修改存档,也不参与游戏运行期的任何行为。

## 2. SD 布局(对外契约)

| 路径 | 内容 |
|---|---|
| `/switch/ACNH-Manager/acnh-manager.nro` | 应用本体 |
| `/switch/ACNH-Manager/state.json` | 安装记录:agent 版本、写入的文件与 sha256、时间 |
| `/switch/ACNH-Manager/settings.json` | 用户设置(目前只有界面语言) |
| `/switch/ACNH-Manager/log.txt` | 运行日志(含失败原因;M1 起产生) |
| `/switch/ACNH-Manager/spike.log`、`spike-history.log` | M0 环境探测输出(仅 spike 构建产生) |
| `atmosphere/contents/01006F8002326000/exefs/subsdk9` | agent 模块(跨构建共享) |
| `atmosphere/contents/01006F8002326000/exefs/main.npdm` | 按游戏构建派生的 NPDM(每个构建一份) |
| `atmosphere/contents/01006F8002326000/exefs/acnh-agent.version` | 安装记录侧车 |

不写 `SHA256SUMS`;卸载**按记录里的路径删除,不再校验 sha256**(玩家手工改过的文件也要能删掉:
拒删比误删更伤用户),每个条目都会尝试,删不掉的汇总成一条原因返回;目录为空时才删除目录。
两阶段安装在启动时清理 `.acnh-` 家族的崩溃残留(见 §4)。

设置与安装记录**分开存**:`state.json` 是安装记录,卸载会删掉它;界面语言必须活过卸载,所以
它有自己的文件(`ui::Settings` 负责 `settings.json` 的解析/序列化,主机测试钉住往返)。文件缺失
或读不动都不是错误 —— 用默认值,原因写进 `log.txt`。

### 2.1 写盘错误码:`0xD401` = 内核 `InvalidMemoryState`

解码:libnx 的 `MAKERESULT(Module_Kernel=1, 106)` = `1 | (106 << 9)` = `0xD401`
(`src/libnx/nx/include/switch/result.h` 里 `KernelError_InvalidMemoryState=106`),**不是** fs
自己的 `PathAlreadyExists`(`0x402`,Atmosphere `fs_results.hpp` 里 desc=2)。也就是说:调用本身
合法,但**内核认为这次内存操作的当前状态不对**。

真机上这个码来自三种情形,前两种是"目标已经在那儿了",第三种是最坑的:

1. `mkdir` 一个**已存在**的目录;
2. 备份/落位时 rename 的目标**已存在**(FAT 上 rename 不能覆盖);
3. **路径字符串贴着映射末尾**,导致 IPC 窗口伸进未映射页(2026-09-18 定位,详见第 4.2 节)——
   这一种看起来像"fs 会话失联",实际是缓冲区位置问题。

因此写盘层的规则是:**不要用错误码猜语义**。建目录失败先查目录是否真的在;备份/落位失败先查
源与目标的实际状态;并且整次安装走两阶段提交(先全部写临时文件并校验,再统一 rename 落位,失败
整体回滚),这样即使遇到未记录的结果码,也不会把用户留在"半装"状态。回滚本身也不能靠错误码:
FAT 上 rename 不能落在**已存在**的路径(新文件正躺在那里),所以恢复备份前必须先删掉目标,否则
rename 静默失败、备份留在原地(真机实测见 `docs/device-acceptance.md` 的故障注入一节)。

## 3. 版本识别与 payload 选择

真机实测(见第 5 节)确认 `fsp-ldr` 对普通自制程序不可用,所以判据是下面这条链;为什么宁可
fail-closed 也不用"最接近的匹配",理由见第 6.2 节里每种状态的处理方式:

```text
① ns   nsListApplicationContentMetaStatus(01006F8002326000)
        → Application/Patch 的 version(期望 2228224)与 Patch 所在 storage(期望 SD=5)
② ncm  ncmOpenContentMetaDatabase(SD) + GetLatestContentMetaKey(01006F8002326800) + ListContentInfo
        → 更新标题的 content meta:version 必须 == 2228224、type == Patch(0x81);
          其内容表中 type=1(Program)的 content id 必须 == E10617820DB06889E1638499478DA0DE
          (NCA 内容哈希,内容级指纹:与构建一一对应,与主机无关)
③ (可选,游戏进程存在时)dmnt:cht → main 模块 ModuleId(期望 FF1D1C05…),用于字节级复核
④ 最终防线:agent 自身的代码指纹 fail-closed
```

- ①+② 是默认门控(游戏不需要运行,相册 applet 模式即可);③ 是在玩家已启动游戏时的加固手段。
- `(titleId, version, content id)` 命中 manifest 的 `games[]` 条目 → 安装该条目列出的文件。
- `subsdk9` 跨构建共享,`main.npdm` 按构建各一份;未命中条目或读取失败默认拒绝安装。
- `fsp-ldr`/`OpenCodeFileSystem` 路线在实测中被拒(细节见第 5 节),不再作为依赖。
- 注意区分两个 content id:`ncm` 里更新标题(type=1)的 `E1061782…` 才随构建变化;
  `lr` 解析 base title id 得到的 `80CBC793…` 是**基础标题 1.0.0 的 Program NCA**(version 0),
  对所有更新版本都一样,不能用作门控(实测确认)。

## 4. SD 写入约定(实现约束)

`fs` 服务**不允许写到 EOF 之外**:对刚创建的空文件直接 `fsFileWrite` 不会扩展文件,写入静默失败
(错误码在返回值里,只看屏幕看不出问题)。所有对 SD 的写入都必须按下面的顺序:

```text
fsFsDeleteFile(路径)                     # 需要重建时
fsFsCreateFile(fs, 路径, 0, 0)           # 或按最终长度创建
fsFsOpenFile(fs, 路径, FsOpenMode_Write, &file)
fsFileSetSize(&file, 目标结束偏移)        # ← 关键步骤,每次写入前把文件撑到目标长度
fsFileWrite(&file, 当前偏移, 数据, 长度, FsWriteOption_Flush)
fsFileFlush(&file) / fsFileGetSize(&file, &size)   # 收尾与自检
```

同样适用于安装引擎写 `subsdk9` / `main.npdm` / `acnh-agent.version`:先写临时文件并 `SetSize`,
校验后再改名落地。参考实现:`src/sphaira/source/fs.cpp` 的 `write_entire_file`(CreateFile →
OpenFile → SetSize → Write)。

### 4.1 路径必须是以 `/` 开头的绝对路径

`fs*` 系列只接受绝对路径。清单里的 `files[].target` 按契约是相对路径(`atmosphere/...`,
`manifest::IsSafeTarget` 明确禁止前导 `/`),**直接把它交给 `fsFsCreateFile` 会被 FS 拒绝**:
真机实测返回 `0x2EEA02`(module=2 FS,description=6005),而且只有 `create` 会失败 —— 读、校验
(干跑)全都正常,表现为"干跑 3/3 通过、真装第一步就失败"。

写盘探针(`/switch/ACNH-Manager/dev-writeprobe`,见第 6.5 节)在同一台机器上给出的矩阵:

| 目标 | 路径写法 | create 结果 |
|---|---|---|
| `/switch/ACNH-Manager/` | 绝对 | `0x00000000`,write/read/delete 全通过 |
| `switch/ACNH-Manager/` | 相对 | `0x002EEA02` |
| `/atmosphere/contents/01006F8002326000/exefs/` | 绝对 | `0x00000000`,write/read/delete 全通过 |
| `atmosphere/contents/01006F8002326000/exefs/` | 相对 | `0x002EEA02` |

结论有两条:①相对路径就是这个错误码的唯一原因;②**applet 模式可以写 SD 卡,包括游戏目录**
(所以不需要为了写盘去要 application 模式)。引擎因此把所有落到 `fs*` 的路径都过一遍
`install::AbsolutePath()`(读、写、哈希、改名、删除、建目录),清单里的相对 target 只在
`state.json` 与清单里保持相对形态。

### 4.2 路径必须放在有富余的缓冲里(否则 `0xD401`)

libnx 的 `fs*` 把路径当作**声明长度为 `FS_MAX_PATH`(0x301)的 IPC 缓冲**传出去 —— 不管字符串
实际多长 —— 而服务端会**按这个声明长度映射一整段页对齐的窗口**。于是:只要路径字符串落在某个
映射末尾的 0x301 字节内,这段窗口就会伸进没有映射的页,内核直接返回 `InvalidMemoryState`
(`0xD401`),而那个目录其实好端端地在那儿。

真机实测(设备端逐调用追踪,见 `docs/device-acceptance.md` 的"路径贴着映射边界"一节):

```text
fsop: mkdir rc=0x0000D401 ptr=0x329C697E80 region=0x329C674000+0x24000
fsop:   window=0x329C697E80..0x329C698181 region_end=0x329C698000 last_page type=0x0(unmapped)
```

窗口比映射末尾多出 0x181 字节,最后那一页是未映射的。同一次会话里,凡是窗口留在自己映射内的
调用(`region_end` 远在窗口之后)**全部成功**,包括同一个目录上的 create/delete/opendir ——
所以它不是卡的问题,也不是"fs 会话失联",而是**这个指针的位置**。哪个分配会落在映射末尾由
分配器决定,因此 `std::string` 拼出来的路径有时好有时坏,重启应用后又换一个位置,表现为"时好
时坏、失败点还会在文件之间漂移"。

**规则:交给 `fs*` 的缓冲必须自己盖住整个声明窗口** —— `[buf, buf + FS_MAX_PATH)` 要落在那块缓冲
自己的字节里。两条做法缺一不可:路径先复制进 `util::FsPath`(`source/util/fs_path.hpp`,长度
`2 × FS_MAX_PATH` 的局部缓冲);缓冲本身就是数组时(局部变量或结构体成员),数组也**必须 ≥
`FS_MAX_PATH`**,并用 `static_assert` 钉住尺寸。引擎、环境探测、主程序、日志与开发开关检查现在都
照此办理;新增对 SD 的访问时同样如此 —— 不要把调用者的指针直接递下去。

**平台自己就是这么做的**:在带符号的官方 SDK NSO 里核对过
`nn::fs::detail::FileSystemServiceObjectAdapter::DoCreateFile` —— 它把路径拷进自己的一块
`FS_MAX_PATH` 零填充缓冲,长度装不下时返回 fs 的 `TooLongPath`(6003),然后把**这块缓冲**按完整
声明长度发出去;既从不转发调用者的指针,也从不缩小声明长度。也就是说"把声明改成
`strlen(path) + 1`"不是官方形状,本仓库不采用。

libnx 侧那份报告已被上游移到**非公开仓库**(链接对普通账号不可见),本仓库看不到它、也不跟踪它
的状态:上面这条规则是本仓库自己的保险,不依赖上游是否修改。

## 5. M0 环境探测(spike)

M0 的目标是在真机上把上节调用链钉死,并留下可复核日志。行为:

1. 挂载 SD,重建 `/switch/ACNH-Manager/spike.log`(并追加 `spike-history.log`);
2. 打印 HOS 版本、applet 类型与目标 title id;
3. `nsListApplicationContentMetaStatus(01006F8002326000)` 列出每条 content meta 的类型/存储/版本;
4. 只读列出 `atmosphere/contents/01006F8002326000/exefs/` 现有覆盖文件与大小;
5. 若游戏进程存在,用 `dmnt:cht`(65003 + 65002)读 `main` 的 ModuleId 做交叉校验;
6. 服务可获取性表(`fsp-ldr`/`fsp-srv`/`lr`/`ncm`/`pl:u`/`dmnt:cht`/`spl:` 的 open 与 domain 转换);
7. `fsp-ldr` 四步拆解(smGetService → ConvertToDomain → SetCurrentProcess → OpenCodeFileSystem);
8. 备选取 FS 路径:`lr` 解析程序路径、`fsOpenFileSystemWithPatch/WithId`(type 0/1/8)、
   `fsOpenDataFileSystemByProgramId`;
9. `ncm` 列出目标标题的 content meta 与内容 id(与 `lr` 路径里的 id 交叉复核)。

spike 只读:不写游戏目录,不创建 `state.json`。

### 实测记录

| 项 | 结果 |
|---|---|
| 运行日期 / 环境 | 2026-09-16~17,HOS 22.1.0,HOS raw `0x00160100` |
| 首次运行日志为空 | 已定位并修复:写文件前必须 `fsFileSetSize`(见第 4 节);修复后每次运行都完整落盘 |
| `nsListApplicationContentMetaStatus` | Application(storage=5,version=0)、Patch(storage=5,**version=2228224**)、3 个 AddOnContent |
| `dmnt:cht`(游戏运行中) | `program_id=01006F8002326000`,`module_id=FF1D1C05670DB6021C85B624A710B963` —— 与预期一致 |
| `fsp-ldr` | 不可用:相册 applet 模式与应用模式(R 键 title override)下 `smGetService("fsp-ldr")` 均返回 `0x615`,与游戏是否运行无关;四步拆解确认失败在第 1 步 |
| 其他服务 | `fsp-srv`/`lr`/`ncm`/`pl:u`/`dmnt:cht`/`spl:` 均可打开(`fsp-srv` 还能 domain 转换,故排除"服务访问被整体拒绝") |
| `fsOpenFileSystemWithPatch/WithId`(type 0/1/8) | 全部被拒(`0xfdeee202` 等);`fsOpenDataFileSystemByProgramId` 同样被拒 |
| `lr` 程序路径 | 可用:`@SdCardContent://registered/00000010/80cbc793d37b6f493ba53561106bb5c6.nca`(这个 id 是基础标题的 Program 内容,不能当门控,见下一行) |
| 现有 exefs 覆盖 | `subsdk9` 105 837 B、`main.npdm` 1 652 B、`acnh-agent.version` 219 B |
| 冷启动未进游戏 | 重启整机、不进游戏直接运行 spike(applet 模式,`application running=0`):`ns` 版本、`ncm` 数据库与内容列表**全部可用**;`ncm[update]` 给出 version 2228224(type=0x81)与 type=1 内容 id `E10617820DB06889E1638499478DA0DE` ✔ |
| `lr` 与 `ncm` 的关系 | `lr` 解析 base title id 返回 `@SdCardContent://registered/00000010/80cbc793d37b6f493ba53561106bb5c6.nca`,而 `ncm` 显示 `80CBC793…` 属于**基础标题**(version 0)的 Program 内容;因此门控改用 `ncm` 的更新内容 id |
| 指纹绑定(游戏运行中,同一次运行) | `dmnt:cht` 的 ModuleId `FF1D1C05670DB6021C85B624A710B963` 与 `ncm[update]` 的 version 2228224 / type=1 内容 id `E10617820DB06889E1638499478DA0DE` 同时成立;`tools/summarize-spike-log.py` 判定 gate ①+② 与 hardening ③ 均 PASS |

## 6. 核心逻辑模块(M1 起)

判据与决策已从探针代码抽成**不依赖 libnx 的纯逻辑**,主机侧可直接测试;真机部分只负责"取数"和"落盘"。

| 模块 | 职责 |
|---|---|
| `source/util/json.*` | 极简 JSON 子集解析与输出(对象/数组/字符串含 `\uXXXX` 与代理对/数字/布尔/null),对象保持插入顺序。**通用基础设施**:清单、安装记录、设置文件三处都用它,所以放在 `util/` 而不是清单模块里 |
| `source/manifest/manifest.*` | 发布清单解析与**严格校验**:schema、`agent.dirty==false`、`agent.buildFlags==` 发布位(仅语义钩子位)、`target` 路径安全、`sha256` 64 位十六进制、`size>0`、`restart` 取值、`app.minVersion`;任一项不符即整体拒绝 |
| `source/install/gate.*` | 门控判定 `Evaluate()`、安装决策 `Plan()`、`state.json` 的序列化/解析 |
| `source/env/config_ini.*` | `override_config.ini` 与 per-title `config.ini` 的语义解析,产出"启动游戏要不要按键"的人话提示(纯逻辑) |
| `source/env/detect.*` | 真机取数(只读):`ns` 内容表、`ncm` 更新标题 Program 内容 id、`dmnt:cht` ModuleId、覆盖配置、exefs 现状与旧金手指检测 → `EnvironmentReport` |

### 6.1 覆盖键提示的精确语义(实测澄清)

`override_config.ini` 里有两套键,含义不同:

- `[default_config] override_key`(本机为 `!L`)= **exefs 覆盖**(cheats / `subsdk9`)是否生效的开关:
  `!L` 表示默认生效、按住 L 关闭。这是"覆盖键提示"那一行真正描述的东西。
- `[hbl_config] override_any_app_key`(默认 `R`)+ `override_any_app`(默认 `true`)= 按住该键
  **启动应用**时改为进入 hbmenu。

第二条常被误读成"任何程序"。Atmosphere 的判据是
`override_any_app && ncm::IsApplicationId(program_id)`(`cfg_override.board.nintendo_nx.inc`),
而"应用"的定义是 **`0x0100000000010000` ~ `0x01FFFFFFFFFFFFFF`**(`ncm_content_meta_id.hpp`):

| 启动对象 | id | 分类 | 按住 R 的结果 |
|---|---|---|---|
| 游戏(如 ACNH `01006F8002326000`) | 应用段内 | Application | 进入 hbmenu(以应用身份运行) |
| Nintendo eShop | `010000000000100B` | SystemApplet(`...1000`–`...1FFF`) | **无效果** |
| 游戏新闻 / 设置 / 相册 | 同段 | SystemApplet | **无效果** |

所以界面文案写"启动**游戏**会进入 hbmenu"是准确的(玩家会在意的就是游戏),系统程序不在其列;
本机实测与源码判据一致。

### 6.2 门控状态(`Evaluate`)

| 状态 | 触发条件 | 处理 |
|---|---|---|
| `Supported` | 命中 `games[]` 条目:title 与 version 相同、内容 id 相同;若游戏在运行且条目有 `buildId`,ModuleId 也必须相同 | 允许安装 |
| `NoManifest` | App 没有可用清单(M1 阶段即如此,清单由 M4 导入) | 不写盘,提示"清单不可用" |
| `TitleNotSupported` | 清单里没有该 title | 拒绝 |
| `VersionNotSupported` | title 有但版本不同 | 拒绝 |
| `ContentIdMissing` | `ncm` 读不到内容 id(权限或异常) | 拒绝(fail-closed,不做"降级为仅版本匹配") |
| `ContentIdMismatch` | 版本相同但内容指纹不同(重打包/未支持的新构建) | 拒绝 |
| `BuildIdMismatch` | 游戏正在运行,且 `dmnt:cht` 的 ModuleId 与条目不符 | 拒绝 |

### 6.3 安装动作(`Plan`)

按顺序判定:门控未通过 → `Blocked`;无安装记录 → `Install`;记录的内容 id 不同 → `Install`(换构建);
记录的 agent 版本不同 → `Install`(升级/降级);任一文件缺失或 sha256/size 不符 → `Repair`;
全部一致 → `UpToDate`(跳过写入)。

`Plan()` 只比较**记录与清单**,不看卡 —— 这是它保持纯逻辑、可主机测试的前提。**卡上到底有没有
那些文件,由 `install::VerifyInstalledFiles()` 在每次 `Collect()` 之后核对**(记录里每个文件都要
在、且 sha256 一致),不一致就把动作降级成 `Repair` 并把原因写进日志。少了这一步,玩家在应用
之外删掉/改名 `exefs` 之后,首页会一直说"已安装"(真机上被这么报过;验收记录在
`docs/device-acceptance.md`)。

### 6.4 state.json

```json
{"schema":1,"agentVersion":"0.11.0","agentCommit":"4ac89fd403b7",
 "contentId":"E10617820DB06889E1638499478DA0DE",
 "buildId":"FF1D1C05670DB6021C85B624A710B963",
 "installedAt":"2026-09-17T00:00:00Z",
 "files":[{"target":"atmosphere/contents/01006F8002326000/exefs/subsdk9","size":105837,"sha256":"…"}]}
```

写入必须遵守第 4 节的 `SetSize` 约定;这份记录也是"卡上应该有什么"的清单,每次 `Collect()` 会
拿它核对卡(见 6.3),核对不上就按 `Repair` 处理。

### 6.5 测试与阶段说明

- 主机测试:`make -C tests`(覆盖 JSON 解析/拒绝、清单校验、路径安全、版本比较、门控七种状态、
  安装四种动作、state 与 settings 往返、首页八态分类、触摸判定、页眉命中矩形、覆盖键语义多种情形)。
- 清单与内嵌 payload 由导入工具从 acnh-agent 的发布产物生成(门控与落盘清单见
  `docs/release-process.md` 第 2 节)。**没导入时**状态页显示"未内置(等待发布导入)",App 仍能
  启动并显示环境,只是无法安装。
- **M0 探针仍随 App 保留**:在 SD 卡上创建空文件 `/switch/ACNH-Manager/dev-probe` 时,App 会在
  环境报告之后额外运行 M0 取证的探针(写 `spike.log`),用于复核 `fsp-ldr` 等已否路线。
- **写盘探针**:存在 `/switch/ACNH-Manager/dev-writeprobe` 时,启动阶段会对"绝对/相对 × 普通目录/
  游戏目录"四种组合各跑一遍 create→SetSize→write→flush→read→delete,把每步返回码写进 `log.txt`
  (见第 4.1 节)。它只创建并删除自己的临时文件,不碰已有文件;定位完写盘问题后应把开关删掉。
- **分段暂停**:存在 `/switch/ACNH-Manager/dev-pause` 时,首帧之前等一次 `+`,之后每画完一段
  (填充/页眉/正文/页脚)再等一次 —— 用来逐段定位渲染崩溃。开关删掉即恢复成"启动直接出界面"。
- **文本界面**:存在 `/switch/ACNH-Manager/ui-text` 时,不走图形界面,退回控制台文本界面
  (`source/ui/text_ui.cpp`)。图形路径排查时的兜底入口。
- **触摸探针**:存在 `/switch/ACNH-Manager/dev-touchprobe` 时,首帧画完后激活触摸屏并在
  `log.txt` 里记录每次按下/移动/抬起的坐标(实测结论见 `docs/device-acceptance.md`),
  用来确认 applet 模式的触摸可用性与坐标空间。定位完应把开关删掉。
- **fs 会话探针**:存在 `/switch/ACNH-Manager/dev-fsprobe` 时,启动阶段与**每次安装/卸载失败
  之后**各跑一遍 `probe::RunFsSessionProbe()`,并在安装/卸载过程中把引擎的每一次 `fs*` 调用
  (`install::SetFsTraceSink`)连同路径指针、所在映射区间、窗口末尾是否越界写进 `log.txt`。
  第 4.2 节的根因就是靠它定出来的;排查"安装失败 / 怀疑卡或会话有问题"时打开,平时删掉。

## 7. 界面层(M1)

| 模块 | 职责 |
|---|---|
| `source/ui/gfx.*` | framebuffer 上的最小绘制层:填充、矩形、描边、带 alpha 的像素混合 |
| `source/ui/font.*` | FreeType + 主机共享字体(`plGetSharedFontByType`,Standard/简中/扩展简中/繁中/韩文),带按字号分组的字形缓存;缺字自动换下一款字体 |
| `source/ui/app.*` | 页面状态机(首页 / 详情 / 安装确认 / 卸载确认 / **进度** / 结果)、输入处理、卡片式布局与渲染;首页状态由 `ui/home_state.hpp` 的八态分类决定 |
| `source/ui/settings.*` | 用户设置(`settings.json`,目前只有界面语言)的解析与序列化(纯逻辑,主机可测) |
| `source/i18n/strings.*` | 简中 / English 双语文案表(`StringId` 枚举 + 两列),由主机测试保证两边都补齐 |
| `source/util/text_wrap.hpp` | 折行规则(纯函数,主机可测):拉丁文本按空格断词、超长单词才中段断、CJK 按字断、`\n` 强制换行 |

- 链接依赖:`-lfreetype -lharfbuzz -lpng -lbz2 -lz -lnx`(portlibs 里的静态 FreeType 自带
  harfbuzz auto-hinter、PNG 与 bzip2 支持,四个库都必须显式列出),头文件在
  `$(PORTLIBS)/include/freetype2`。
- 渲染循环:`framebufferCreate` + `framebufferMakeLinear`,每帧 `framebufferBegin/End`。
  **绘制高度只能取 `win->height`,不能取 `height_aligned`**:线性影子缓冲是按窗口高度分配的
  (`stride * ((win->height + 7) & ~7)`),`height_aligned` 却是 GOB 向上取整(720 → 768 行);
  按它绘制每帧会越界写堆 48 行(约 245 KB),破坏与 hbl 加载器共享的进程堆,表现为
  "进入游戏或按键就整机崩溃"。宽度用 `width_aligned` 是安全的(1280 本来就对齐)。
- 交互(现状):首页 `A` 执行主操作(安装 / 重装 / 重试 / 已是最新时刷新)、`X` 检查更新、
  `Y` 卸载;`L/R` 切"状态 / 详情";详情页 `A` 切换界面语言、`Y` 开关"允许开发清单";
  `B` 返回、在状态页再按一次退出(或按 `+`)。安装/确认/进度/结果/卸载各自一页,共用同一套渲染。
- **输入统一走动作表**(`source/ui/action.hpp`,纯逻辑 + 主机测试):一个控件 = `{id, 矩形,
  是否可用}`。触摸用 `HitTest` 命中它,方向键/摇杆用 `MoveFocus` 在同一行/列内移动焦点,
  `A` 执行当前焦点 —— 触屏与按键是同一套动作的两条通路。触摸本身在 `source/ui/touch.*`:
  只初始化一次,之后每帧读 `hidGetTouchScreenStates`,坐标即屏幕坐标(实测见
  `docs/device-acceptance.md`);没有触摸(座机模式/服务不可用)时按钮依旧可用。
- **点击判定在 `TapTracker`**(同文件):触摸面板**只在手指接触时给出坐标**,松手那一帧
  `state.count == 0`、没有位置可言,所以"按下期间的最后坐标 + 最大偏移"必须由自己记住 ——
  拿松手帧的坐标去做拖拽判定,会让每次点击都被当成拖出屏幕的滑动而静默丢弃(真机踩过,
  `docs/device-acceptance.md` 有复现数据)。规则:全程偏移 ≤ 32 px、且抬手点仍在同一个启用
  动作上才触发;滑动(偏移超限)一律不触发。
- **画出来的按钮必须能点**。页眉的 `L 状态` / `R 详情` 与页脚的 `Ⓑ` 提示都是按钮:它们和
  对应按键走同一条 `ActivateAction` 路径,命中矩形与绘制几何共用同一个布局函数
  (`ui/header_tabs.hpp` 的 `LayoutHeaderTabs`,页脚的 `LayoutFooterHint`),避免"画得到、
  点不到"再次发生(真机收到过这个反馈)。这些外壳控件在每页自己的控件**之后**追加,进入页面
  仍落在主按钮上;只读页(详情)开头清空动作表,点空白处不会误触上一页的按钮。
  **唯一的例外是进度页**:安装/卸载期间引擎占着这条线程,页面上不放任何控件,也不处理按键,
  于是它连页眉标签与页脚提示都不画(画了就是"画得到、点不到"的反面教材)。

### 7.1 绘制与排版约定

界面出过的两类事故都来自"约定没写下来",所以这里定死:

- **两套 Surface,坐标约定不同,名字即约定**。`Clipped(x, y, w, h)` 只收缩绘制范围,
  坐标系仍是绝对像素(用来给整页兜底,例如"不许画到页脚上");`Subview(x, y, w, h)`
  把原点挪到该矩形左上角,子视图内用**局部**坐标(用来把一段内容锁进卡片/列表)。
  两者都与上一层裁剪自动求交,所以页级限制会传递下去。混用两者会让整块内容被静默裁掉
  (卡片框还在、正文全空),因此卡片正文、文件列表一律用 `Subview`。
- **折行只有一份实现**。`ui/font.*` 的 `Wrap` 是唯一折行逻辑,`Draw`、`Measure`、
  `LineCount`、`Fit` 都从它派生,保证"量出来的行数"与"画出来的行数"一致。
  布局按量出来的行数算行高和卡片高度,而不是写死行距 —— 长值折行后压到下一行、
  或正文越过卡片下边框,根因都是这两者不一致。
- **断词规则在 `util/text_wrap.hpp`**:拉丁文本在空格处断(放不下的词整体下移),
  只有单个超长单词才允许中段断;CJK 没有空格,保持按字断。这条规则单独抽成纯函数是因为
  它无法靠眼睛验收 —— 真机出现过英文句子被劈成 `Note: hol` / `ding R` 的情况,
  现在有主机测试逐条钉住(含这个具体用例)。
- **长文本按行截断,完整内容进日志**。`Fit(text, size, width, max_lines)` 超出时在末行
  补省略号;清单校验原文这类开发者向信息用小一号字(`Row::size`),界面只画得下的部分,
  完整文本写进 `log.txt`。
- **界面语言是进程级的**。`i18n::SetLanguage()` 跟着界面切换走,安装引擎、网络层等
  非 UI 模块用 `i18n::Text` / `i18n::Format` 取文案;凡是**采集时就拼好的句子**
  (如覆盖键建议)必须在切语言后重新采集一次,否则会留着旧语言的文本(实测过)。
- **页面装不下时先收紧行距**。三张卡片的行间距在 `kRowGap`(默认 8)与 0 之间自动下调;
  卡片高度由内容算出,最后一张补满剩余空间但绝不越过页脚。日志会打印每次布局的实际值
  (`status: layout row_gap=… needed=… available=…`),判断"排版是不是被压紧了"看这一行。

## 8. 安装 / 卸载引擎(M2)

| 模块 | 职责 |
|---|---|
| `source/util/sha256.*` | 自带 SHA-256(纯 C++17),标准测试向量校验,支持分块流式更新 |
| `source/util/time.*` | Unix 秒 → `YYYY-MM-DDTHH:MM:SSZ`(纯函数) |
| `source/install/engine.*` | 落盘与记录:目录递归创建、整文件读写、分块哈希、`WriteFileVerified`(临时文件 → `SetSize` → 写入 → 回读校验 → 备份改名 → 删备份)、`state.json` 读写、`Install()`/`Uninstall()` |

### 8.1 写入与替换顺序(不写半截文件)

```text
阶段一(不碰已装文件):payload 读取 → 大小比对 → sha256 比对
  → 每个目标各写一份 .acnh-tmp:CreateFile → SetSize → Write(Flush) → 回读 sha256
阶段二(只剩改名):目标已存在则先改名 .acnh-old → .acnh-tmp 改名到目标
  → 三个文件都落位后写 state.json(同样先写 .acnh-tmp 再改名)
  → 全部成功后才删除所有 .acnh-old
```

阶段一失败:还没有碰过已装文件,只清掉自己写的临时文件即可,**卡片逐字节不变**。
阶段二失败:已经落位的文件用 `RestoreBackup()` 逐个恢复(`.acnh-old` 换回目标),
`state.json` 写不进去时同样回滚三个游戏文件 —— 记录与文件永远成对,不会出现"文件是新的、
记录还是旧的"这种状态。恢复用"先删目标再改名"而不是直接改名:rename 落在已存在路径上会失败
(见 §2.1),那样会把备份留在游戏目录里。任一步失败都立即返回错误,下次运行由 `Plan()` 判定为
`Repair`。

安装与卸载的结果(成功与否、写入/删除几个文件、失败原因)都会写进 `log.txt`:
`install: ok=… files=… error=…` / `uninstall: …`。失败原因过去只出现在结果页上,用户报障时
无法从日志回看 —— 故障注入测试期间正好撞上过一次,所以现在两条路径都留痕。

**曾经看起来像"会话失联"的那个失效模式**已经定位并修掉:某些序列之后同一会话里成片失败、错误码
恒为 `0xD401`,真因是**路径字符串贴着映射末尾**导致 IPC 窗口越界(第 4.2 节),现在所有路径都走
`util::FsPath`。`install::IsStaleSessionError` 与结果页那句"退出本程序后重新打开,再试一次"
(`i18n::StringId::ResultRelinkHint`)作为兜底保留:真出现这个签名时,重启应用是实测有效的动作。
复现步骤、逐调用追踪数据与修复验证见 `docs/device-acceptance.md` 的"路径贴着映射边界"一节。

### 8.2 卸载

按 `state.json` 里的路径**逐条删除,不校验 sha256**:玩家手工改过的文件(比如自己换了 agent)
也必须能删掉,拒删比误删更伤用户。每个条目都会尝试——删到一半停下会留下"既不完整也删不掉"的
目录——删不掉的汇总成一条原因返回。全部删成功后,`exefs` 目录只在为空时删除(非空说明还有别人的
文件),最后删掉 `state.json`。

### 8.3 清单来源与"开发用清单"

- **正式通道(内嵌)**:`tools/import-agent-release.py` 通过门控后把清单与 payload 写入 `data/`,
  构建时由 devkitPro 的 bin2s 变成 NRO 里的 `.rodata` 符号(`source/payload/embedded.*` 读它们)。
  所以正式安装完全离线,且不需要 romfs、devoptab 或任何服务 —— 这条路径在本机上被反复验证是安全的。
- **开发通道(SD)**:`/switch/ACNH-Manager/dev-manifest.json` 与 `/switch/ACNH-Manager/payload/`
  由 `tools/make-dev-manifest.py` 从 acnh-agent 的构建产物生成;只在正式通道不可用、且详情页
  显式**允许**(详情页按 `Y` 开关"允许开发清单")时才读;开发通道与开发 payload 必须成对出现。
  它是我们自己的调试入口,
  不是用户路径:**发布包、商店包、指南站下发物与用户卡上都不应包含这两个对象**,开发卡上的这份
  必须与当前发布版一致(把 `packaging/agent/<版本>/` 的三个产物与 `manifest.json` 推到
  `payload/` 与 `dev-manifest.json`),否则一旦内嵌通道缺失,回退安装的是过期 payload。
  允许开发清单时才会被读到。App 用两处文案区分来源("已内置(NRO 自带)" / "开发用清单文件")。
- 清单校验默认要求"发布形态"(`dirty=false` 且 `buildFlags` 只含语义钩子位);开发构建会被
  **默认拒绝**并在状态页显示原因。要在 M4 之前干跑,必须在详情页显式打开"允许开发清单",
  此时 UI 会标注清单来自开发文件 —— 这是有意的:发布门控不接受非发布产物。
  这个开关**不持久化**(每次启动回到默认的"拒绝开发清单"):它是调试入口,不该跟着用户走。
- payload 与清单同源:内嵌清单配内嵌 payload,开发清单配 SD 上的 payload 目录,不会混用。
- `干跑模式`只做校验与统计,不写任何文件;图形界面**不提供**这个开关(用户只关心安装与卸载),
  它留在开发用的文本界面(`ui-text`,默认开启,按 `Y` 切换;那个界面里开发清单开关是 `ZL`)。
  两边的确认页都会列出将要写入的文件与大小。

## 9. 联网检查更新与一键更新

一次检查要做三件事:把清单取回来、证明它是我们发的、必要时把它当升级来源用。每一段各管一件事:

| 模块 | 职责 |
|---|---|
| `source/net/http.*` | 本仓库唯一的 HTTPS 传输(libcurl + mbedTLS,套在 libnx 的 BSD socket 服务上)。只接受 `https://`;连接 5s、总 8s 超时;正文有上限(清单 512 KiB、payload 4 MiB);失败以**数据**返回(outcome + 原文诊断),从不抛异常或终止进程;**TLS 证书校验关闭**(原因见 9.1) |
| `source/net/signature.*` | mbedTLS 验签(ECDSA P-256),公钥来自编进 NRO 的 `data/agent_pubkey.bin` |
| `source/net/update.*` | 拉清单 → 拉同址的 `.sig` → 验签 → 解析;返回 `UpdateOutcome` + 原文诊断(玩家侧措辞由界面按当前语言渲染) |
| `source/net/update_task.*` | 工作线程(128 KiB 静态栈):检查在独立线程上跑,界面每帧 `Take()` 一次结果 |
| `source/install/network_source.*` | 按 `baseUrl + files[].source` 下载 payload;落盘的信任来自引擎自己的校验(每个文件先对清单里的 size + sha256) |

### 9.1 为什么可以关掉 TLS 校验

与这台机器上的其它自制软件一致(Sphaira 的下载代码同样是 `CURLOPT_SSL_VERIFYPEER 0` +
`VERIFYHOST 0`),原因是**这条技术栈根本没有信任锚**:mbedTLS 设计上不带任何根证书,libcurl 的
Switch 版够不到系统自己的证书库(在 Atmosphere 的 `ssl` 服务后面,libnx 的 `ssl.h` 里能看到
DigiCert / ISRG / GlobalSign 等一长串),而镜像里的 libcurl 是 7.69.1,早于
`CURLOPT_CAINFO_BLOB`(7.71),内嵌 bundle 只能落成 SD 上的文件、还得我们自己负责更新。
(2026-09-18 的决定:关闭校验;2026-09-19 起用清单签名补上内容可信。)

关键不是"连接可信",而是"内容可信":能写进游戏目录的每一个字节都由清单的 sha256 定死,
而清单本身必须先通过验签 —— 见 9.2。中间人换得掉连接,换不出一个能过验签的清单。

### 9.2 清单签名(内容可信的根)

- 私钥只存在发布机上,放在本地密钥目录 `~/.acnh/acnh-manager-signing-key.pem`(权限 600,不入库,
  要备份);公钥 `data/agent_pubkey.bin` 编进 NRO,随 NRO 一起发给玩家;
- 这把公钥同时在两处留痕:发布锁的 `signingPublicKeySha256`(自检比对,让换钥匙在 diff 里显式
  出现)与 NRO 的 `.rodata`(自检核对 NRO 里编的确实是当前那把);
- 签名文件与清单同址:URL 加后缀 `.sig`(`agent-manifest.json` → `agent-manifest.json.sig`),
  格式就是 `openssl dgst -sha256 -sign` 的 DER;
- 验签失败(缺签名 / 不通 / 本 build 没带公钥)= 清单不可用:不解析、不显示版本、更不会下载;
- 因此**每次发布都必须重新签名**清单,否则线上每一个 App 都会把它当垃圾丢掉
  (`tools/sign-manifest.py`,流程见 `docs/release-process.md` 第 6 节);
- 换私钥等于换身份:已发出去的 App 只认旧公钥,新清单会全部验签失败,只能靠换 NRO 才能追上。

### 9.3 界面行为

- **启动时静默检查一次**:有新版首页自己变成"更新到 X";失败静默降级为内置版本
  (原因只进 `log.txt` 与详情页,不打扰玩家);
- 首页按 `X` 手动检查,结果写在按钮副标题上(进行中画 spinner);无论成功失败都写上;
- 检查跑在工作线程上,**整个界面在检查期间照常响应**(改之前是同步调用,整个 App 会卡住数秒);
- 远端清单只在**比本机已装版本更新、并且门控接受本机构建**时才会成为安装来源(规则是
  `net/update_policy.hpp` 里的纯函数,由主机测试钉住):更新就是"下载 → 按清单校验 → 安装"。
  只"更新"但不再覆盖本机构建的版本**不会**顶掉自带发布,而是如实显示"新版 X 不支持当前游戏
  版本";否则一台本来可用的机器会被自己的更新检查判成"不支持"(2026-09-20 真机复现);
- **安装期间有进度页**:引擎在帧循环线程上跑,所以 `Page::Progress` 在第一字节之前先画一帧、
  每处理完一个文件再画一帧(`BeginProgress`/`UpdateProgress`),这张页面上没有可点的控件、也不
  处理按键——它只是把"应用卡住了"变成"正在处理 subsdk9(1/3)";
- 工作线程建不起来(例如 `threadCreate` 返回 `0x1759`)也按失败上报,而不是让按钮变成"按了没反应"。

### 9.4 语义边界

- **检查失败不影响安装**:装什么由内嵌(或开发通道)清单决定,联网只提供"有没有更新的版本";
- 目标地址是 `https://gitee.com/acnh-game/acnh-manager/raw/main/agent-manifest.json`
  (仓库根目录那份,由导入工具随发布记录一起刷新,与商店包、内嵌 payload 同源);
- 地址是编译期常量:换托管方 = 重新发一版 NRO。

## 10. 退出路径与 hbl 的进程复用

相册里的 hbl 加载器**整个相册会话只用一个进程**:它把每个 NRO 重定位到同一进程里跑,
NRO 返回后就在原地换下一份代码。任何没交还给系统的资源都会留在那个进程里,跨 NRO 累积。

真机实测(2026-09-17,同一相册会话):连续"启动本应用 → 退出"到第三次时,返回加载器菜单那一步
只剩黑屏;此时应用自己的日志是完整干净的(`ui: first frame done` → `ui: loop exited` →
`app: exiting`),hbl / 相册进程仍然活着,系统连主界面都画不出来(按 Home 后是黑屏/白屏),
`capssc` 截屏全黑——也就是说进程没崩,是显示资源被耗尽了。第一、二次退出都正常;同一台机器上
换成"每次退出后都回主界面"(下面的 exit mode)连续六次启动/退出都干净通过。

代码层面的解释:libnx 启动时(`__nx_win_init`)通过 `appletCreateManagedDisplayLayer` 向 applet
管理器申请一个受管显示层,退出时只调用 `viCloseLayer`(关闭该层的缓冲队列),而**受管层的销毁
由 applet 管理器在 applet 终止时执行**(`vi:m` 的 `DestroyManagedLayer`,由 am 的
DisplayLayerManager 在 applet 结束时调用)。hbl 复用进程这一件事,让"每次加载都新建一层、
但只有 applet 结束才回收"变成了累积;同样的路径对所有 NRO 都成立,不只是本应用。

因此本应用在 `source/main.cpp` 里定义 `__nx_applet_exit_mode = 1`:退出时走 applet 的退出
命令(与 DBI 的做法一致),让 applet 管理器终止这个 applet 并回收它持有的全部资源。代价是
退出后回到主界面而不是回到加载器菜单,这是有意的取舍——相册黑屏对玩家的代价大得多。

验证方式(真机):连续做"主界面 → 相册 → Sphaira → ACNH-Manager → 退出"六次以上,每次退出
都应干净回到主界面,过程中不出现黑屏/白屏;对比基线是同一台机器上 `__nx_applet_exit_mode = 0`
的构建,它会在第 3~4 次退出后让相册黑屏。

排查这类问题时的**取数方式**(不用保存副本):hbl 本体就在卡上 `/atmosphere/hbl.nsp`
(PFS0,里面是 `main`(NSO)与 `main.npdm`),用工作区里的 `src/hactool` 解开就能看它的 NSO /
NPDM —— 我们核对加载器自身的退出与显示行为时解过一次;解出来的二进制属过程材料,不入库。
