# ACNH-Manager — AGENTS.md

本文件是 `src/acnh-manager` 的专属规则;它是 acnh-agent 的官方安装渠道(自制 NRO 应用),属于工作区
(`/Users/leo/Documents/switch 金手指`)的独立 Git 仓库。环境级事实(Docker 镜像、密钥政策、
Switch 构建纪律)与跨仓库命名规则见工作区根 `AGENTS.md`;产品侧定稿方案见
`../../docs/acnh_manager_plan.md`(开工前必读,本文只写仓库内规则)。

## 定位与边界

- 系列级产品壳:面向 Switch / Switch 2 上的动物森友会作品,**当前仅支持 ACNH 3.0.3**;对外文案
  (`README.md`、商店条目)必须写明这一点。
- 命名:`acnh-` 家族前缀(`nh` = Nintendo Homebrew),显示名 `ACNH-Manager`。
- 写入 SD 的位置是对外契约,单一来源是 `docs/architecture.md` 第 2 节(改路径前先改那份文档,再改代码与商店 manifest)。
- 不修改游戏存档、不管理 cheat 文件(只检测并提示)、不自动部署、不自动上架。

## 构建

```bash
./tools/build.sh     # 固定镜像 devkitpro/devkita64:20260219;Docker 需 escalation
```

Docker socket 受沙盒限制,需 escalation;构建成功不等于授权部署,真机安装/测试由用户手工执行。
链接依赖(见 `docs/architecture.md` 第 7 节):`-lfreetype -lharfbuzz -lpng -lbz2 -lz -lnx`。

## 验证

声称完成前至少跑:① `make -C tests`(主机侧单元测试,纯逻辑改动必跑);② `./tools/build.sh` 必须零警告通过;
③ `python3 -m py_compile tools/*.py`;④ 涉及真机结论时,用 `python3 tools/summarize-spike-log.py <日志>` 复核
(它按当前判据给出 PASS/FAIL)。

## 纪律

- 只读参考 `src/libnx`、`src/Atmosphere-src`、`src/acnh-agent`;不要在本仓库复制它们的源码。
- 内嵌 payload 只能来自 acnh-agent 通过门控的发布产物:**发布走 `tools/release.sh`**(容器内现场构建
  agent 发布版,agent 仓库只读、`dist/` 不被碰),由导入工具写入 `packaging/agent/<版本>/`(发布记录,
  原始文件名)与 `data/`(bin2s 构建输入,必须带 `.bin` 后缀)。不得手工拷入未校验的二进制;
  发布后跑 `tools/verify-release.py --nro acnh-manager.nro` 核对四处一致。
- 过程性材料(spike 日志、截图、构建输出)放已忽略的 `build/scratch/`,不进 `docs/`。
- 新增/改名/删除工具或文档时,同一次改动内更新 `docs/tools-guide.md` 与本文档导航。

## 语言约定

- **源码性质的文件全英文**(注释也算):`source/`、`tests/`、`tools/`、`Makefile` 里的注释、日志、
  诊断与脚本输出一律英文 —— 改动别人的英文文件时不要混进中文。
- **例外是 i18n**:`source/i18n/strings.cpp` 的中文列是产品数据,允许保留(全仓库仅此一处)。
  文档类文件(`AGENTS.md`、`README.md`、`docs/**`)不受限制。
- 新增用户可见文案一律进 i18n 表(两列都填),不要在代码里直接写中文句子 —— 安装引擎与网络层
  用 `i18n::Text` / `i18n::Format` 取文案,语言由 `i18n::SetLanguage` 跟着界面语言走。

## 目录规划

- `source/`:应用本体。M0 的探针(`main.cpp`/`probe.*`)不是一次性代码——`probe.*` 在 M1 演进为
  环境检查模块,`main.cpp` 的控制台壳将来换成界面壳。
- `source/manifest/`:清单解析与校验(纯逻辑,可主机测试);`source/install/`:门控判定、安装决策与
  `state.json`(同上);`source/payload/`:内嵌发布通道(读 `data/` 里 bin2s 生成的符号);
  `source/ui/`:界面层;`source/net/`:联网检查。
- `data/`:内嵌发布清单与 payload 的 bin2s 构建输入(由导入工具生成,入库)。
- `packaging/agent/<版本>/`:发布记录(subsdk9 / main.npdm / acnh-agent.version / manifest.json),
  与 `data/` 同源,也是指南站要托管的那四个文件;`packaging/agent-lock.json` 是发布锁。
- `tools/`:开发与真机迭代工具(见 `docs/tools-guide.md`)。
- `tests/`:主机侧单元测试(`make -C tests`);`packaging/`:M4 放商店打包与 `pkgbuild.json`;
  `docs/device-acceptance.md`:M5 写真机验收记录。

## 文档导航

| 路径 | 内容/何时读 |
|---|---|
| `README.md` | 项目概览与当前支持范围 |
| `docs/architecture.md` | 架构、版本识别与门控、SD 布局、环境检查(改行为前必读) |
| `docs/tools-guide.md` | 本仓库工具的用法与登记 |
| `docs/release-process.md` | 发布流程:导入门控、商店打包、上架与指南站同步(发布前必读) |
| `docs/store-listing.md` | 商店条目元数据与素材要求(改对外文案前) |
| `docs/device-acceptance.md` | 真机验收矩阵与基线环境(每次真机验收后更新) |
| `../../docs/acnh_manager_plan.md` | 产品定稿方案(跨仓库决策) |
