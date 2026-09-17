# 工具指南

所有工具都在本仓库根目录运行;只有 `tools/build.sh` 需要 Docker(受沙盒限制,需 escalation),
`tools/make-icons.py` 需要带 Pillow 的 Python 运行时(用 Codex 自带运行时,见工作区根 `AGENTS.md`),
其余为纯标准库。

| 工具 | 作用 | 何时用 |
|---|---|---|
| `tools/build.sh` | 在固定镜像 `devkitpro/devkita64:20260219` 里执行 `make`,产出 `acnh-manager.nro` | 每次改完源码构建时 |
| `tools/deploy-nro.py` | 经 sys-agent 的内置 FTP(默认 `switch:6001`)把 NRO 推到 `/switch/ACNH-Manager/`;`--fetch-log` 取回 `spike.log`、`spike-history.log`、`log.txt`(后者 M1 起产生) | 真机迭代:构建 → 部署 → 启动 → 回读日志 |
| `tools/summarize-spike-log.py` | 解析环境探测日志:打印 `ns` 内容表、`ncm` 的更新标题 version 与 Program 内容 id、`dmnt:cht` 的 ModuleId,按当前判据(①ns 版本 ②ncm 内容 id ③ModuleId 复核)给出 PASS/FAIL;`fsp-ldr` 相关行仅作历史信息 | 每次真机探测后复核结论 |
| `tools/make-icons.py` | 从主图 `assets/icon-org.png` 生成 `assets/icon.jpg`(256×256 JPEG,NACP/hbmenu 图标)与 `assets/icon.png`(256×256 PNG,商店图标);`--source` 可换主图;需带 Pillow 的运行时 | 换图标或商店素材时 |
| `tools/make-dev-manifest.py` | 从 acnh-agent 的 `dist/` 产物生成开发用清单与 payload 目录(`build/scratch/dev-payload/`),保留真实的 `buildFlags`/`dirty` | M4 之前做真机干跑;发布门控默认会拒绝这类清单 |
| `tools/deploy-nro.py --push-file <本地> --remote <相对路径>` | 把单个文件推到 `/switch/ACNH-Manager/<相对路径>`(推 payload 与开发用清单用),上传后校验大小 | 部署 NRO 之外的文件时 |
| `tools/import-agent-release.py` | 发布门控 + 导入:校验 `dirty=false`/`buildFlags=2`/NSO 哈希/NPDM 重放校验/profile 指纹,通过后写 `packaging/agent-lock.json` 与内嵌 payload | 每次发布前;开发构建会被拒绝(设计如此) |
| `tools/make-store-package.py` | 生成官方商店 `pkgbuild.json`、图标/横幅与本地测试仓库(`repo.json` + zip) | 打包上架材料或验证商店流程时 |

主机侧测试不在 `tools/` 下,单独放在 `tests/`:`make -C tests` 编译并运行清单解析、门控判定、
安装决策与 `state.json` 往返的单元测试(不需要 Docker 与真机)。

构建产物与日志等过程性材料放 `build/scratch/`(已忽略);真机验收结论写入 `docs/device-acceptance.md`(M5 建立)。
