# 商店条目材料

条目元数据的**单一来源**是 `packaging/listing.json`(中英双语摘要/详情/changelog),
`tools/make-store-package.py` 由它生成 `pkgbuild.json` 与本地测试仓库 `repo.json`。

| 字段 | 值 | 说明 |
|---|---|---|
| package / 目录名 | `acnh-manager` | 也决定图标 URL `packages/acnh-manager/icon.png` |
| title | `ACNH-Manager` | 商店搜索按 title/author/description 子串匹配,玩家搜 `ACNH` 或 `manager` 都能命中 |
| author | `acnh-game` | 与 GitLab 命名空间、NRO 内嵌作者(`Makefile` 的 `APP_AUTHOR`)保持一致 |
| category | `tool` | 商店过滤器里的 Tools |
| license | GPLv3 | 官方要求源码公开 |
| url | `https://gitlab.com/acnh-game/acnh-manager` | 源码与发布页(GitLab;官方只要求公开可访问) |
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
- [ ] 发布页上的 `acnh-manager.nro` 与仓库里构建的产物哈希一致,且位在
      `https://gitlab.com/acnh-game/acnh-manager/-/releases/v<版本>/downloads/acnh-manager.nro`
      —— `pkgbuild.json` 的 update 资产就指向这个 permalink(见 `tools/make-store-package.py`)。
      形状已核对(2026-09-19,参照 GitLab 上 `gitlab-org/cli` 等项目的 Release 资产):
      tag 必须叫 `v<版本>`,NRO 要以**资产链接**附在该 Release 上、filepath 为 `acnh-manager.nro`
      (网页端"Release assets"上传或 `glab release create --assets-links` 都行);
- [ ] 本地测试仓库(`repo.json` + Sphaira 自定义商店源)能完成搜索→安装→更新→卸载;
- [ ] PR 里带上 `icon.png`(必需)与 `screen.png`/截图(推荐)。
