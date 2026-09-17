# 架构

## 1. 职责

ACNH-Manager 是 `acnh-agent` 的安装器:识别控制台上游戏的实际构建,把对应 payload 写入 SD 卡,
并给出安装状态、失败原因与卸载回滚。它不修改存档,也不参与游戏运行期的任何行为。

## 2. SD 布局(对外契约)

| 路径 | 内容 |
|---|---|
| `/switch/ACNH-Manager/acnh-manager.nro` | 应用本体 |
| `/switch/ACNH-Manager/state.json` | 安装记录:agent 版本、写入的文件与 sha256、时间 |
| `/switch/ACNH-Manager/log.txt` | 运行日志(含失败原因;M1 起产生) |
| `/switch/ACNH-Manager/spike.log`、`spike-history.log` | M0 环境探测输出(仅 spike 构建产生) |
| `atmosphere/contents/01006F8002326000/exefs/subsdk9` | agent 模块(跨构建共享) |
| `atmosphere/contents/01006F8002326000/exefs/main.npdm` | 按游戏构建派生的 NPDM(每个构建一份) |
| `atmosphere/contents/01006F8002326000/exefs/acnh-agent.version` | 安装记录侧车 |

不写 `SHA256SUMS`;卸载只删除 `state.json` 记录且 sha256 与记录一致的文件,目录为空时才删除目录。

## 3. 版本识别与 payload 选择

判据、失败处理与理由见 `../../docs/acnh_manager_plan.md` 第 4 节(M0 阶段为纯只读探测)。
真机实测(见第 5 节)确认 `fsp-ldr` 对普通自制程序不可用,因此判据改为下面这条链:

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

写盘探针(`/switch/ACNH-Manager/dev-writeprobe`,见第 6.4 节)在同一台机器上给出的矩阵:

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
| `source/manifest/json.*` | 极简 JSON 子集解析与输出(对象/数组/字符串含 `\uXXXX` 与代理对/数字/布尔/null),对象保持插入顺序 |
| `source/manifest/manifest.*` | 发布清单解析与**严格校验**:schema、`agent.dirty==false`、`agent.buildFlags==` 发布位(仅语义钩子位)、`target` 路径安全、`sha256` 64 位十六进制、`size>0`、`restart` 取值、`app.minVersion`;任一项不符即整体拒绝 |
| `source/install/gate.*` | 门控判定 `Evaluate()`、安装决策 `Plan()`、`state.json` 的序列化/解析 |
| `source/env/config_ini.*` | `override_config.ini` 与 per-title `config.ini` 的语义解析,产出"启动游戏要不要按键"的人话提示(纯逻辑) |
| `source/env/detect.*` | 真机取数(只读):`ns` 内容表、`ncm` 更新标题 Program 内容 id、`dmnt:cht` ModuleId、覆盖配置、exefs 现状与旧金手指检测 → `EnvironmentReport` |

### 6.1 门控状态(`Evaluate`)

| 状态 | 触发条件 | 处理 |
|---|---|---|
| `Supported` | 命中 `games[]` 条目:title 与 version 相同、内容 id 相同;若游戏在运行且条目有 `buildId`,ModuleId 也必须相同 | 允许安装 |
| `NoManifest` | App 没有可用清单(M1 阶段即如此,清单由 M4 导入) | 不写盘,提示"清单不可用" |
| `TitleNotSupported` | 清单里没有该 title | 拒绝 |
| `VersionNotSupported` | title 有但版本不同 | 拒绝 |
| `ContentIdMissing` | `ncm` 读不到内容 id(权限或异常) | 拒绝(fail-closed,不做"降级为仅版本匹配") |
| `ContentIdMismatch` | 版本相同但内容指纹不同(重打包/未支持的新构建) | 拒绝 |
| `BuildIdMismatch` | 游戏正在运行,且 `dmnt:cht` 的 ModuleId 与条目不符 | 拒绝 |

### 6.2 安装动作(`Plan`)

按顺序判定:门控未通过 → `Blocked`;无安装记录 → `Install`;记录的内容 id 不同 → `Install`(换构建);
记录的 agent 版本不同 → `Install`(升级/降级);任一文件缺失或 sha256/size 不符 → `Repair`;
全部一致 → `UpToDate`(跳过写入)。

### 6.3 state.json

```json
{"schema":1,"agentVersion":"0.11.0","agentCommit":"4ac89fd403b7",
 "contentId":"E10617820DB06889E1638499478DA0DE",
 "buildId":"FF1D1C05670DB6021C85B624A710B963",
 "installedAt":"2026-09-17T00:00:00Z",
 "files":[{"target":"atmosphere/contents/01006F8002326000/exefs/subsdk9","size":105837,"sha256":"…"}]}
```

写入必须遵守第 4 节的 `SetSize` 约定;卸载只删除这里记录且哈希一致的文件。

### 6.4 测试与阶段说明

- 主机测试:`make -C tests`(覆盖 JSON 解析/拒绝、清单校验、路径安全、版本比较、门控七种状态、
  安装五种动作、state 往返、覆盖键语义多种情形)。
- 清单与内嵌 payload 由 M4 的导入工具从 acnh-agent 的发布产物生成(发布门控见
  `../../docs/acnh_manager_plan.md` 第 8 节)。**没导入时**状态页显示"未内置(等待发布导入)",App 仍能
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

## 7. 界面层(M1)

| 模块 | 职责 |
|---|---|
| `source/ui/gfx.*` | framebuffer 上的最小绘制层:填充、矩形、描边、带 alpha 的像素混合 |
| `source/ui/font.*` | FreeType + 主机共享字体(`plGetSharedFontByType`,Standard/简中/扩展简中/繁中/韩文),带按字号分组的字形缓存;缺字自动换下一款字体 |
| `source/ui/app.*` | 页面状态机(状态 / 设置)、输入处理、卡片式布局与渲染 |
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
- 交互:`A` 刷新环境(设置页为切换语言)、`L/R` 切页、`B` 或 `+` 退出;安装/确认/进度/结果页
  在 M2 用同一套渲染接入。

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
payload 读取 → 大小比对 → sha256 比对(先验后写)
  → 目标 .acnh-tmp:CreateFile → SetSize → Write(Flush) → 回读 sha256
  → 旧文件改名 .acnh-old → 临时文件改名到目标 → 删除 .acnh-old
  → 全部文件成功后写 state.json(同样走上面的校验流程)
```

任何一步失败都立即返回错误,`state.json` 不更新,下次运行会通过 `Plan()` 判定为 `Repair`。

### 8.2 卸载

按 `state.json` 逐条校验 sha256:**改过的文件拒绝删除**(不猜测、不强制),全部通过后逐个删除;
`exefs` 目录只在为空时删除(非空说明还有别人的文件);最后删掉 `state.json`。干跑模式只做校验与统计。

### 8.3 清单来源与"开发用清单"

- **正式通道(内嵌)**:`tools/import-agent-release.py` 通过门控后把清单与 payload 写入 `data/`,
  构建时由 devkitPro 的 bin2s 变成 NRO 里的 `.rodata` 符号(`source/payload/embedded.*` 读它们)。
  所以正式安装完全离线,且不需要 romfs、devoptab 或任何服务 —— 这条路径在本机上被反复验证是安全的。
- **开发通道(SD)**:`/switch/ACNH-Manager/dev-manifest.json` 与 `/switch/ACNH-Manager/payload/`
  由 `tools/make-dev-manifest.py` 从 acnh-agent 的构建产物生成;只在正式通道不可用、且设置页显式
  允许开发清单时才会被读到。App 用两处文案区分来源("已内置(NRO 自带)" / "开发用清单文件")。
- 清单校验默认要求"发布形态"(`dirty=false` 且 `buildFlags` 只含语义钩子位);开发构建会被
  **默认拒绝**并在状态页显示原因。要在 M4 之前干跑,必须在设置页显式打开"允许开发清单",
  此时 UI 会标注清单来自开发文件 —— 这是有意的:发布门控不接受非发布产物。
- payload 与清单同源:内嵌清单配内嵌 payload,开发清单配 SD 上的 payload 目录,不会混用。
- `干跑模式`(设置页默认开启)只做校验与统计,不写任何文件;确认安装页会同时显示将要写入的
  文件、大小与 sha256 前缀。

## 9. 联网检查更新(M3)

| 模块 | 职责 |
|---|---|
| `source/net/update.*` | 用 libcurl + mbedTLS 拉取发布清单;强制证书校验(`CURLOPT_CAINFO` 指向 CA bundle),连接 5s / 总 8s 超时,响应上限 512 KiB |

行为约定:

- **只检查,不决策**:拿不到清单时静默保留内置/本地清单,并记录原因;失败绝不影响安装门控;
- **不做"关闭证书校验"的降级**:没有 CA 文件就直接跳过并说明(`跳过: 缺少 CA 文件`);
- 目标是 `https://lextuo.com/acnh-chat-code/guide/agent-manifest.json`(与指南站、商店包同一份);
- CA 来源:内嵌进 NRO(与 payload 同一条 bin2s 通道,不引入 romfs)或读 `/switch/ACNH-Manager/ca.pem`;
- 触发方式:目前是设置页按 `+` 手动触发(阻塞式,数秒)。**启动时静默检查需要工作线程**,
  留到 M5 与界面一起收尾。
