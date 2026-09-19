# 发布流程

目标:把一个**已验证的** acnh-agent 构建与安装器一起发布到官方 Homebrew App Store,并保持
指南站、商店包、NRO 内嵌 payload 三处版本一致。

## 1. 一条命令跑完整条链

```bash
./tools/release.sh                  # 全流程(docker 需要提权)
./tools/release.sh --skip-agent-build --skip-store
./tools/release.sh --allow-dirty    # 只用于开发期验证链本身,不构成发布
```

链条(全部在本仓库内执行,acnh-agent 只以**只读**方式参与):

| 步 | 做什么 | 关键约束 |
|---|---|---|
| 0 | 前置检查 | **两个仓库都必须干净**(本仓库脏 → 构建戳会带 `-dirty`;agent 脏 → 产品带 dirty=true 被门控拒);原始 NPDM 在;docker 在 |
| 1 | 固定镜像里构建 acnh-agent 发布形态(`make switch SEMANTIC_HOOK=1`) | agent 仓库 `:ro` 挂载,容器里先拷到 `/work` 再构建 —— **它的 `dist/` 不会被读写** |
| 1b | 校验暂存产物来自当前 agent HEAD | 挡住"`--skip-agent-build` 时拿旧产物配新 HEAD 发布":产物自带 `version.json.commit` 与 `commit.txt` 都必须等于 HEAD |
| 2 | 用 acnh-agent 的 `make-minimal-npdm.py` 从原始 NPDM 派生 `main.npdm` | 原始 NPDM 来自 agent 的 `research/`(只读),产物写进本仓库 `build/scratch/` |
| 3 | 发布门控 + 导入 | 见第 2 节 |
| 4 | 构建 NRO(`data/` 经 bin2s 进 `.rodata`) | 与普通构建同一条命令 |
| 5 | 自检 `tools/verify-release.py` | 见第 3 节 |
| 6 | 打商店包 | `tools/make-store-package.py` |

前置:acnh-agent 工作树干净、`research/` 里有原始 `main.npdm`、docker 可用;改完代码后先跑
`make -C tests`。

`--allow-dirty` 是开发期开关(例如只想验证链本身能否跑通):它只把"本仓库干净"这一条降级成警告,
打出的 NRO 构建戳会带 `-dirty` —— 看到那个标志就说明这份产物不是发布件。

## 2. 门控与导入(第 3 步的细节)

```bash
python3 tools/import-agent-release.py --dry-run \
    --nso <构建产物>/acnh-agent.nso --npdm <派生目录>/main.npdm \
    --version-json <构建产物>/version.json --original-npdm <原始 main.npdm>
```

`--nso` / `--npdm` / `--version-json` **都是必填**:发布必须来自刚构建出来的产物,不接受
`acnh-agent/dist/` 里的现成文件(手工跑时也得自己给路径)。

门控(任一不过即拒绝,退出码 2):`dirty=false`、`buildFlags=2`、NSO 哈希与 `version.json` 一致、
**派生 NPDM 重放校验**(用同一个原始 NPDM 再派生一次、逐字节比对)、profile 的
`(titleId, buildId)` 一致。落盘内容:

```
packaging/agent-lock.json          发布锁(版本/commit/开关/文件哈希/构建指纹)
packaging/agent/<agentVersion>/    发布记录,保留原始文件名(对外托管的就是这四个):
                                     subsdk9 / main.npdm / acnh-agent.version / manifest.json
data/manifest.bin                  内嵌发布清单(json 文本)
data/subsdk9.bin                   内嵌 payload 文件(清单里 source=subsdk9)
data/main_npdm.bin                 内嵌 payload 文件(source=main.npdm)
data/acnh_agent_version.bin        内嵌 payload 文件(source=acnh-agent.version)
agent-manifest.json                仓库根目录的"当前版本"清单(发布记录的副本;
                                   App 的更新检查按 raw 地址读它)
```

发布记录与 `data/` 由同一次导入写出,内容逐字节对应;`data/` 只是改名后的构建输入。
`data/` 是 devkitPro 的 DATA 目录:构建时 bin2s 把每个文件变成 `.rodata` 里的符号
(`subsdk9.bin` → `subsdk9_bin` / `subsdk9_bin_size`),由 `source/payload/embedded.cpp` 读取,
所以正式通道不需要 romfs / devoptab / 任何服务。**DATA 目录里的文件必须带 `.bin` 后缀**
(构建规则是 `%.bin.o: %.bin`),文件名里的点会变成下划线。

当前已导入的发布:`acnh-agent 0.11.0`(commit `8541a0956c30`,`buildFlags=2`)。
acnh-agent 的 `dist/` 默认是开发构建(`buildFlags=7`,带 RPC server),会被门控拒绝——这是设计如此,
不要用手工数据绕过它;要出正式产物必须先 `make switch SEMANTIC_HOOK=1` 重新构建。

## 3. 自检

```bash
python3 tools/verify-release.py --nro acnh-manager.nro
```

核对四处一致:发布锁必须仍是发布形态;`packaging/agent/<版本>/` 里三个文件的哈希与锁一致、
`acnh-agent.version`/`manifest.json` 的内容与锁一致;`data/` 里的内嵌 trio 与发布记录逐字节相同;
给了 `--nro` 时还要求 **NRO 里确实找得到内嵌清单与三个 payload 的原始字节**,并且 **NRO 的构建戳
对得上当前源码树**(戳里的 `src:<8 hex>` 与现场重算的 `source/` 哈希一致)。
这是唯一能抓住"手上这个 NRO 嵌的不是锁里那一份"的检查,可以单独挂到 CI。

第二条尤其重要:只查 payload 是抓不到"**源码改了但 NRO 没重建**"的(内嵌字节没变,自检照样全过)。
实测:往 `source/` 里放一个无关文件后,戳从 `src:445e4876` 变成 `src:bd493d78`,自检立刻报
`NRO 是当前源码树构建的` 失败。戳带 `-dirty` 时只提示不失败 —— 那是"构建时工作树不干净"的标记。

### 3.1 产物哈希的稳定性(实测)

同一提交、同一镜像、同一条命令构建两次 → 字节一致(可复现);**换拷贝方式也一致**:发布链用
`tar` 顺序与"反序逐个拷贝"两种方式构建,产物哈希相同。

这条性质来自 acnh-agent 侧的一处修复(2026-09-17):它的 `misc/mk/common.mk` 原先直接用
`find`/`$(wildcard ...)` 列源文件,而这两个返回的是**目录项顺序**,`OFILES` 顺序即链接顺序,
于是同一提交在不同文件系统下会链接出不同布局 —— 实测同一提交同一镜像:挂载构建 74 062 B
(`978a90b6…`)、拷贝构建 73 978 B(`1dd59c9b…`),两者目标文件集合(105)与符号集合(178)
完全相同,只有 67 个符号地址整体平移。修复后 `MODULES`/`SOURCES`/`CFILES`/`CPPFILES`/`SFILES`/
`BINFILES` 全部排序,`make audit` 会拦住未排序的 `wildcard`;细节见 acnh-agent 的
`docs/versioning.md` §4.1。

因此:**发布记录里的哈希可以在任何机器上重建比对**;只是要记得,任何一次源码改动(哪怕是注释)
都会换掉哈希,所以对账用的是"当前提交 + 发布锁",不是跨提交的绝对数值。

## 3. 打包商店材料

```bash
./tools/build.sh                          # 产出 acnh-manager.nro
python3 tools/make-store-package.py       # 产出 build/scratch/store/
```

产物与用途:

| 路径 | 用途 |
|---|---|
| `packages/acnh-manager/pkgbuild.json` | 提交给官方数据仓库 `forusers/switch-hbas-repo` 的元数据(update 资产指向本项目的 GitLab Release) |
| `packages/acnh-manager/icon.png`、`screen.png` | 商店图标与横幅 |
| `zips/acnh-manager.zip` | 包内容(`switch/ACNH-Manager/acnh-manager.nro` + `manifest.install` + `info.json`) |
| `repo.json` | **本地测试仓库**:与官方 CDN 同布局,可作为 Sphaira 自定义商店源验证"搜索→安装→更新→卸载" |

## 4. 上架官方商店

1. 项目托管在 GitLab `acnh-game/acnh-manager`(公开):打 tag `v<版本>`,把 `acnh-manager.nro`
   作为**资产链接**附到该 Release(网页端 "Release assets" 上传,或
   `glab release create v<版本> ./acnh-manager.nro`),permalink 就是
   `https://gitlab.com/acnh-game/acnh-manager/-/releases/v<版本>/downloads/acnh-manager.nro`;
   `pkgbuild.json` 的 update 资产指向它(形状 2026-09-19 核对过,见 `docs/store-listing.md`);
2. 向 `forusers/switch-hbas-repo` 提 PR:新增 `packages/acnh-manager/{pkgbuild.json,icon.png,screen.png}`;
3. PR 阶段 CI 会构建出一个预览仓库并评论在 PR 里,先按评论自查;
4. 合并到 main 后 CI(spinarak)重建并部署到 `switch.cdn.fortheusers.org`,当天生效;
   客户端受 repo.json 缓存影响,可能需要等一会儿或重新打开商店。

官方商店对"写入 `/atmosphere/contents/<tid>/` 的包"已有先例(`UltimateTrainingModpack`、`SwitchCast`),
但审核有裁量权;被拒或排队期间,**GitLab Release 的直链兜底**必须可用(见第 5 节)。

## 5. 托管与直链(GitLab)

项目全部托管在 GitLab,不再依赖自建服务器;三个对外地址都是 GitLab 自己的地址:

```
# agent 文件与清单:发布记录就提交在仓库里,用 raw 端点直接给出
https://gitlab.com/acnh-game/acnh-manager/-/raw/main/packaging/agent/<agentVersion>/{subsdk9,main.npdm,acnh-agent.version}
# 更新检查读的"当前版本"清单:导入工具每次都会刷新仓库根目录这份
https://gitlab.com/acnh-game/acnh-manager/-/raw/main/agent-manifest.json
# App 自身的下载(商店 update 资产与兜底直链):GitLab Release 资产
https://gitlab.com/acnh-game/acnh-manager/-/releases/v<appVersion>/downloads/acnh-manager.nro
```

要点:

- `packaging/agent/<agentVersion>/manifest.json` 是发布记录,**仓库根目录的 `agent-manifest.json`
  是它的副本**,由 `tools/import-agent-release.py` 在导入时一起写 —— App 的联网检查读后者的
  raw URL(见 `docs/architecture.md` 第 9 节),所以"最新版本"和"发布记录"永远同源;
- 清单里的 `baseUrl` 指向 `packaging/agent/<agentVersion>/` 的 raw 前缀,`files[].source` 相对它解析;
- raw 端点只认**公开仓库 + 已推送的提交**:改完这两个文件必须提交并 push,线上地址才会更新
  (这是"忘了推"最常见的坑,验收时先核对 raw 地址能取到东西);
- 发布记录与指南页兜底都给同一个 NRO:GitLab Release 资产(`docs/store-listing.md` 上架清单有步骤)。

## 6. 版本与记录

- 本仓库版本(`APP_VERSION`)与商店条目 `version` 必须一致;功能更新升 Minor、修复升 Patch、
  特大变更先问用户(工作区规则);
- 每次发布后把结论写进 `docs/device-acceptance.md`:安装了哪个 agent 版本、在什么构建上验过、
  失败用例的观察结果。

## 7. 尚未完成的部分(明确记录)

- **联网检查的定位**:TLS 证书校验按 2026-09-18 的决定关闭(没有信任锚可用,与 Sphaira 一致),
  所以这个检查只报版本、不下载内容。**如果以后要让它在 App 内在线升级 agent**,必须先补上
  TLS 校验或"清单签名 + 内嵌公钥验签"(见 `docs/architecture.md` §9);
- **启动即静默检查**:目前是首页按 `X` 手动触发,改成启动时后台线程(M5);
- **商店收录**:官方 Homebrew App Store 的收录步骤与元数据(见第 3 节)尚未实际提交过一次。
