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
