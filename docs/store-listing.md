# 商店条目材料

条目元数据的**单一来源**是 `packaging/listing.json`(中英双语摘要/详情/changelog),
`tools/make-store-package.py` 由它生成 `pkgbuild.json` 与本地测试仓库 `repo.json`。

| 字段 | 值 | 说明 |
|---|---|---|
| package / 目录名 | `acnh-manager` | 也决定图标 URL `packages/acnh-manager/icon.png` |
| title | `ACNH-Manager` | 商店搜索按 title/author/description 子串匹配,玩家搜 `ACNH` 或 `manager` 都能命中 |
| author | `acnh-game` | 与 Gitee 命名空间、NRO 内嵌作者(`Makefile` 的 `APP_AUTHOR`)保持一致 |
| category | `tool` | 商店过滤器里的 Tools |
| license | GPLv3 | 官方要求源码公开 |
| url | `https://gitee.com/acnh-game/acnh-manager` | 源码与发布页(Gitee;官方只要求公开可访问) |
| 安装路径 | `switch/ACNH-Manager/acnh-manager.nro` | 与 App 数据目录同名 |

## 素材

| 素材 | 现状 | 要求 |
|---|---|---|
| 图标 | `assets/icon.png`(256×256,与 NRO 内图标同源) | 必需;商店网格里要能一眼认出 |
| 横幅 `screen.png` | **待补** | 宽图,详情页顶部展示;可由图标主图裁切或另做一张截图拼版 |
| 截图 | **待补** | 建议 3 张:状态页、安装确认页、卸载页(真机截图) |

## 文案要点(已在 listing.json 中)

- 一句话:安装与管理《集合啦!动物森友会》游戏内 agent 的工具;
- 详情里明确三点:① 写哪些文件;② 只支持 ACNH 3.0.3,不支持的构建会被拒绝;
  ③ 逐文件 SHA-256 校验 + 一键回滚;
- 避免在英文文案里使用 "cheat" 字样:这是安装器,不是作弊包;中文文案同理。

## 上架检查清单

- [ ] `packaging/listing.json` 的中英文案与当前功能一致(尤其"当前支持范围");
- [ ] `assets/icon.png` 是最新设计稿;
- [ ] 仓库里的 `packaging/nro/acnh-manager-<版本>.nro` 与本次构建的产物哈希一致,且位在
      `https://gitee.com/acnh-game/acnh-manager/raw/main/packaging/nro/acnh-manager-<版本>.nro`
      —— `pkgbuild.json` 的 update 资产就指向这个 permalink(见 `tools/make-store-package.py`)。
      形状已核对(2026-09-20:raw 地址匿名可取,会 302 到 `raw.giteeusercontent.com` 的签名地址);
      文件名的版本号来自 Makefile 的 `APP_VERSION`,由 `tools/release.sh` 第 5 步拷入;
- [ ] 本地测试仓库(`repo.json` + Sphaira 自定义商店源)能完成搜索→安装→更新→卸载;
- [ ] PR 里带上 `icon.png`(必需)与 `screen.png`/截图(推荐)。

## 提交前怎么验(2026-09-21 实测流程)

官方商店的构建器是 **`fortheusers/spinarak`**(数据仓库 `fortheusers/switch-hbas-repo` 的 CI 跑的就是它),
它下载 pkgbuild 里的资产、生成 `manifest.install` 与 `info.json`、打包 zip 并汇总 `repo.json`。
**不要凭印象写这几个文件**——直接跑它,用它的产物当基准:

```bash
# 1. 生成我们的包材料(build/scratch/store/)
python3 tools/make-store-package.py

# 2. 造一个只含本包的构建目录,把 pkgbuild 与 icon 放进去
mkdir -p build/scratch/spinarak-check/packages/acnh-manager
cp build/scratch/store/packages/acnh-manager/{pkgbuild.json,icon.png} \
   build/scratch/spinarak-check/packages/acnh-manager/
curl -fsSL -o build/scratch/spinarak-check/spinarak.py \
   https://raw.githubusercontent.com/fortheusers/spinarak/main/spinarak.py

# 3. 在 packages/ 目录下运行它(它会从 Gitee raw 下载 NRO,需要联网)
cd build/scratch/spinarak-check/packages && python3 ../spinarak.py
```

成功时会打印 `Built 1 of 1 packages.`,产物在 `packages/public/`。**逐项对比**:

| 文件 | 必须一致的部分 |
|---|---|
| `zips/<name>.zip` 里的 `manifest.install` | 形如 `U: switch/ACNH-Manager/acnh-manager.nro` —— 客户端按 `line.substr(3)` 取路径,**少一个 `": "` 就会把 `switch/…` 读成 `witch/…`** |
| `zips/<name>.zip` 里的 `info.json` | 九个字段:title/description/author/version/license/url/category/details/changelog |
| `repo.json` 的包条目 | 除 `md5`/`sha256`(它们是各自 zip 的哈希,顺序/时间戳不同就会不同)以外逐字段一致 |

`tools/make-store-package.py` 生成的本地测试仓库就是照着这套规格写的(2026-09-21 重写:早先的版本
写成了 `U <路径>` 且带多余的 `G:` 行,客户端会解析错)。Sphaira 把它当自定义商店源时,走的才是
真实代码路径;如果只是"看着像",这个测试没有意义。
