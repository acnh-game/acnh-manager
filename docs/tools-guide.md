# 工具指南

所有工具都在本仓库根目录运行。需要 Docker 的是 `tools/build.sh`(构建 NRO)与 `tools/release.sh`
(它还要在容器里构建 acnh-agent 的发布版);沙盒里 Docker 需 escalation。
`tools/make-icons.py`、`tools/device-tests.py` 与 `tools/ui-measure.py` 需要带 Pillow 的 Python 运行时
(用 Codex 自带运行时,见工作区根 `AGENTS.md`);其余为纯标准库。`device-tests.py` 还需要真机:
`switch` 可达(sys-agent 6000、其 FTP 6001)、应用已装好、机器停在主界面。

| 工具 | 作用 | 何时用 |
|---|---|---|
| `tools/build.sh` | 在固定镜像 `devkitpro/devkita64:20260219` 里执行 `make`,产出 `acnh-manager.nro` | 每次改完源码构建时 |
| `tools/deploy-nro.py` | 经 sys-agent 的内置 FTP(默认 `switch:6001`)把 NRO 推到 `/switch/ACNH-Manager/`;`--fetch-log` 取回 `spike.log`、`spike-history.log`、`log.txt`(后者 M1 起产生) | 真机迭代:构建 → 部署 → 启动 → 回读日志 |
| `tools/summarize-spike-log.py` | 解析环境探测日志:打印 `ns` 内容表、`ncm` 的更新标题 version 与 Program 内容 id、`dmnt:cht` 的 ModuleId,按当前判据(①ns 版本 ②ncm 内容 id ③ModuleId 复核)给出 PASS/FAIL;`fsp-ldr` 相关行仅作历史信息 | 每次真机探测后复核结论 |
| `tools/make-icons.py` | 从两张主图生成三种素材:`assets/icon-org.png` → `assets/icon.jpg`(256×256 JPEG,NACP/hbmenu 图标)+ `assets/icon.png`(256×256 PNG,商店图标);`assets/screen-org.png`(可选,不入库)→ `assets/screen.png`(**848×208** 商店横幅,按 848:208 居中裁带,绝不拉伸填满;主图不在就跳过)。`--source`/`--banner-source` 可指向别处的主图,`--skip-banner` 跳过横幅;需带 Pillow 的运行时 | 换图标/横幅或商店素材时 |
| `tools/check-i18n.py` | 校验 `StringId` 枚举与 `strings.cpp` 文案表**逐条同序**并打印条目总数(死文案/漏文案会让两边错位);`make -C tests` 已内建调用 | 每次改动 `source/i18n/*` 之后,或本地测试前 |
| `tools/make-dev-manifest.py` | 从 acnh-agent 的 `dist/` 产物生成开发用清单与 payload 目录(`build/scratch/dev-payload/`),保留真实的 `buildFlags`/`dirty` | M4 之前做真机干跑;发布门控默认会拒绝这类清单 |
| `tools/deploy-nro.py --push-file <本地> --remote <相对路径>` | 把单个文件推到 `/switch/ACNH-Manager/<相对路径>`(推 payload 与开发用清单用),上传后校验大小 | 部署 NRO 之外的文件时 |
| `tools/release.sh` | 一条命令跑完整条发布链:前置检查(两个仓库都要干净)→ 容器内构建 agent 发布版 → 校验暂存产物来自当前 agent HEAD → 派生 NPDM → 门控导入 → 构建 NRO → 自检 → 商店包;agent 仓库只读挂载、容器里先拷到 `/work` 再构建,它的 `dist/` 完全不被碰 | 每次发布;`--skip-agent-build/--skip-nro/--skip-store` 跳过单步,`--allow-dirty` 仅开发期验证链用(打出的 NRO 戳会带 `-dirty`) |
| `tools/import-agent-release.py` | 发布门控 + 导入:校验 `dirty=false`/`buildFlags=2`/NSO 哈希/**NPDM 重放校验**(用同一原始 NPDM 再派生并逐字节比对)/profile 指纹,通过后写 `packaging/agent-lock.json`、`packaging/agent/<版本>/`(原始文件名的发布记录)与 `data/`(bin2s 构建输入)。`--nso/--npdm/--version-json` 必填,不吃 `acnh-agent/dist/` 里的现成产物 | 发布链的第 3 步;手工单独跑时也要自己给刚构建出来的路径 |
| `tools/make-signing-key.py` | 生成发布签名密钥对(ECDSA P-256,私钥放仓库外,默认给成 `~/.acnh/acnh-manager-signing-key.pem`),并把公钥写进 `data/agent_pubkey.bin`(入库、编进 NRO)。已有私钥时拒绝覆盖,除非 `--force` | 只做一次:项目第一次发布前;换密钥等于换身份(见 `docs/architecture.md` 9.2) |
| `tools/sign-manifest.py` | 用 `--key`(默认 `~/.acnh/acnh-manager-signing-key.pem`)给根目录 `agent-manifest.json` 签名 → `agent-manifest.json.sig`,先核对私钥与内嵌公钥配套,再用 openssl 复验;`--manifest/--signature` 可指向别处的副本(本地镜像测试用) | 每次发布,导入清单之后、push 之前 |
| `tools/verify-release.py` | 核对"锁 ↔ 发布记录 ↔ `data/` ↔ 已构建 NRO"四处一致,并验一次对外清单的签名;`--nro` 时还会在 NRO 里搜内嵌清单与三个 payload 的原始字节,并校验 **NRO 构建戳的 `src:` 哈希 == 当前源码树**(抓"源码改了但 NRO 没重建") | 发布链的第 6 步;也可单独挂 CI |
| `tools/make-qr.py` | 把《森友物码册》小程序码(`src/acnh-chat-code-miniapp/resources/小程序码.jpg`)采样成模块矩阵,写进 `data/qr_miniapp.bin`(u16 宽高 + 每行位打包),供 App 的“聊天码说明”页用矩形逐格画出;同时输出 `build/scratch/qr-check.png` 便于肉眼复核。需要带 Pillow+numpy 的运行时 | 小程序码更新后(或改采样规则后);换码后请用手机扫一次真机截图确认可扫 |
| `tools/make-store-package.py` | 生成官方商店 `pkgbuild.json`、图标/横幅与本地测试仓库(`repo.json` + zip)。产物按**上游 spinarak 的格式**写(manifest 行 `U: <相对路径>`、`info.json` 九字段、`repo.json` 含 md5/sha256/binary/screens 等),可用上游构建器逐项比对(流程见 `docs/store-listing.md`) | 打包上架材料或验证商店流程时 |
| `tools/device-tests.py` | 真机回归测试两组(判定页面用**页面自身特征**:首页那两个按钮的青色徽章、说明页左下的二维码块、确认/结果页的大按钮,因此页眉入口改名或换语言都不会误判)。**故障注入**:`t1a/t1b`(可清理 / 清不掉的 `.acnh-tmp` 残留)、`t2`(记录写不进去)、`restore`(清注入并重装),`all` 连跑;断言"要么全落位、要么卡片逐字节不变",用 `log.txt` 的 `collect:`/`cleanup:` 行 + 卡上逐文件哈希。**启停循环**:`cycles [--cycles N]` 连续"相册 → 应用 → 退出",每轮都要求日志有首帧与干净退出的两行(守相册黑屏那个回归)。截图落在 `build/scratch/device-tests/` | 改过 `source/install/engine.cpp` 的写盘/回滚逻辑、或改动退出路径(`__nx_applet_exit_mode` / 显示层)之后 |
| `tools/ui-measure.py` | 像素尺子:`bbox`(窗口内墨迹外框与左右内缩)、`lines`(按文本行分段,每行的内缩/高度/宽度)、`runs`(某一行的墨迹段,看左边缘与居中)、`color`(某颜色的外框/中心/像素数)。背景可用 `--bg page\|card\|header\|border` 指名(与 `source/ui/app.cpp` 的调色板一致) | 改过任何排版(间距、居中、内缩)之后;把"看着有点歪"变成数字 |

主机侧测试不在 `tools/` 下,单独放在 `tests/`:`make -C tests` 编译并运行清单解析、门控判定、
安装决策与 `state.json` 往返的单元测试(不需要 Docker 与真机)。

构建产物与日志等过程性材料放 `build/scratch/`(已忽略);真机验收结论写入 `docs/device-acceptance.md`(M5 建立)。
