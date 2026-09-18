# 工具指南

所有工具都在本仓库根目录运行。需要 Docker 的是 `tools/build.sh`(构建 NRO)与 `tools/release.sh`
(它还要在容器里构建 acnh-agent 的发布版);沙盒里 Docker 需 escalation。
`tools/make-icons.py` 与 `tools/device-fault-tests.py` 需要带 Pillow 的 Python 运行时(用 Codex 自带
运行时,见工作区根 `AGENTS.md`);其余为纯标准库。后者还需要真机:`switch` 可达(sys-agent 6000、
其 FTP 6001)、应用已装好、机器停在主界面。

| 工具 | 作用 | 何时用 |
|---|---|---|
| `tools/build.sh` | 在固定镜像 `devkitpro/devkita64:20260219` 里执行 `make`,产出 `acnh-manager.nro` | 每次改完源码构建时 |
| `tools/deploy-nro.py` | 经 sys-agent 的内置 FTP(默认 `switch:6001`)把 NRO 推到 `/switch/ACNH-Manager/`;`--fetch-log` 取回 `spike.log`、`spike-history.log`、`log.txt`(后者 M1 起产生) | 真机迭代:构建 → 部署 → 启动 → 回读日志 |
| `tools/summarize-spike-log.py` | 解析环境探测日志:打印 `ns` 内容表、`ncm` 的更新标题 version 与 Program 内容 id、`dmnt:cht` 的 ModuleId,按当前判据(①ns 版本 ②ncm 内容 id ③ModuleId 复核)给出 PASS/FAIL;`fsp-ldr` 相关行仅作历史信息 | 每次真机探测后复核结论 |
| `tools/make-icons.py` | 从主图 `assets/icon-org.png` 生成 `assets/icon.jpg`(256×256 JPEG,NACP/hbmenu 图标)与 `assets/icon.png`(256×256 PNG,商店图标);`--source` 可换主图;需带 Pillow 的运行时 | 换图标或商店素材时 |
| `tools/check-i18n.py` | 校验 `StringId` 枚举与 `strings.cpp` 文案表**逐条同序**并打印条目总数(死文案/漏文案会让两边错位);`make -C tests` 已内建调用 | 每次改动 `source/i18n/*` 之后,或本地测试前 |
| `tools/make-dev-manifest.py` | 从 acnh-agent 的 `dist/` 产物生成开发用清单与 payload 目录(`build/scratch/dev-payload/`),保留真实的 `buildFlags`/`dirty` | M4 之前做真机干跑;发布门控默认会拒绝这类清单 |
| `tools/deploy-nro.py --push-file <本地> --remote <相对路径>` | 把单个文件推到 `/switch/ACNH-Manager/<相对路径>`(推 payload 与开发用清单用),上传后校验大小 | 部署 NRO 之外的文件时 |
| `tools/release.sh` | 一条命令跑完整条发布链:前置检查(两个仓库都要干净)→ 容器内构建 agent 发布版 → 校验暂存产物来自当前 agent HEAD → 派生 NPDM → 门控导入 → 构建 NRO → 自检 → 商店包;agent 仓库只读挂载、容器里先拷到 `/work` 再构建,它的 `dist/` 完全不被碰 | 每次发布;`--skip-agent-build/--skip-nro/--skip-store` 跳过单步,`--allow-dirty` 仅开发期验证链用(打出的 NRO 戳会带 `-dirty`) |
| `tools/import-agent-release.py` | 发布门控 + 导入:校验 `dirty=false`/`buildFlags=2`/NSO 哈希/**NPDM 重放校验**(用同一原始 NPDM 再派生并逐字节比对)/profile 指纹,通过后写 `packaging/agent-lock.json`、`packaging/agent/<版本>/`(原始文件名的发布记录)与 `data/`(bin2s 构建输入)。`--nso/--npdm/--version-json` 必填,不吃 `acnh-agent/dist/` 里的现成产物 | 发布链的第 3 步;手工单独跑时也要自己给刚构建出来的路径 |
| `tools/verify-release.py` | 核对"锁 ↔ 发布记录 ↔ `data/` ↔ 已构建 NRO"四处一致;`--nro` 时还会在 NRO 里搜内嵌清单与三个 payload 的原始字节,并校验 **NRO 构建戳的 `src:` 哈希 == 当前源码树**(抓"源码改了但 NRO 没重建") | 发布链的第 5 步;也可单独挂 CI |
| `tools/make-store-package.py` | 生成官方商店 `pkgbuild.json`、图标/横幅与本地测试仓库(`repo.json` + zip) | 打包上架材料或验证商店流程时 |
| `tools/device-fault-tests.py` | 真机故障注入:`t1a/t1b`(可清理 / 清不掉的 `.acnh-tmp` 残留)、`t2`(记录写不进去)、`restore`(清注入并重装),也可 `all` 连跑。每一步先截图判定页面、再按键,并用 `log.txt` 的 `collect:`/`cleanup:` 行与卡上逐文件哈希做断言;截图落在 `build/scratch/device-fault-tests/` | 改过 `source/install/engine.cpp` 的写盘/回滚逻辑之后,或排查"安装失败/留残留"时 |

主机侧测试不在 `tools/` 下,单独放在 `tests/`:`make -C tests` 编译并运行清单解析、门控判定、
安装决策与 `state.json` 往返的单元测试(不需要 Docker 与真机)。

构建产物与日志等过程性材料放 `build/scratch/`(已忽略);真机验收结论写入 `docs/device-acceptance.md`(M5 建立)。
