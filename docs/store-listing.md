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
| 横幅 `screen.png` | **待补**(等设计稿) | **848×208 PNG**;商店客户端就是把详情页顶部那块画成 848×208(Sphaira `ui/menus/appstore.cpp`:`banner_vec(70, …, 848.f, 208.f)`),缺图时回退成图标。图里**不要写字** —— 标题与说明由商店自己画,图上再写字会重叠 |
| 截图 | `assets/screenshots/{status,install,done,uninstall}.png`(真机 1280×720,四张) | 商店按 `packages/<name>/screen<N>.png` 取图,所以 `tools/make-store-package.py` 会按 `SCREENSHOT_ORDER` 把它们声明成 `screenshot` 资产,并**额外**在包目录里生成 `screen1..4.png`(本地测试仓库与 CDN 布局用)。上游 README 明确说这类构建期文件可以不入 PR |

### 横幅提示词(交给画图标的那位/作图工具)

> 为 Switch 自制软件 "ACNH-Manager" 画一张商店横幅,输出 **848×208 像素的 PNG**(宽高比约 4.08:1),
> 不透明背景,**画面里不要出现任何文字**。
>
> 主题与风格:和现有 App 图标同一套"包裹 + 向下箭头 + 海滩"扁平矢量插画风格(图标主图在
> `assets/icon-org.png`,1254×1254,可作为风格参考),线条干净、柔和阴影、整体可爱但不幼稚。
>
> 配色沿用家族色:主色青绿 `#2FBFA8`,浅薄荷 `#D5F2EC`,奶油背景 `#F6F1E3`,卡片白 `#FFFDF7`,
> 沙色描边 `#D8CFB6`,墨色 `#2B2B2B`,点缀绿 `#1B7F5A`。
>
> 构图:主体居中偏左、四周留白,左右各留约 5% 安全边(有的客户端会轻微裁切);元素要大而简,
> 因为实际显示只有 848×208 这么大。可以直接把图标的包裹/箭头主元素横向延展成一条"海滩上的
> 传送带"意象,也可以只保留图标主体 + 青绿渐变背景。

## 文案要点(已在 listing.json 中)

- 一句话:安装与管理《集合啦!动物森友会》游戏内 agent 的工具;
- 详情里明确三点:① 写哪些文件;② 只支持 ACNH 3.0.3,不支持的构建会被拒绝;
  ③ 逐文件 SHA-256 校验 + 一键回滚;
- 避免在英文文案里使用 "cheat" 字样:这是安装器,不是作弊包;中文文案同理。

## 上架检查清单

- [ ] `packaging/listing.json` 的中英文案与当前功能一致(尤其"当前支持范围");
- [ ] `assets/icon.png` 是最新设计稿;
- [x] 截图已就位(`assets/screenshots/`,真机四张);横幅仍待设计稿;
- [ ] 仓库里的 `packaging/nro/acnh-manager-<版本>.nro` 与本次构建的产物哈希一致,且位在
      `https://gitee.com/acnh-game/acnh-manager/raw/main/packaging/nro/acnh-manager-<版本>.nro`
      —— `pkgbuild.json` 的 update 资产就指向这个 permalink(见 `tools/make-store-package.py`)。
      形状已核对(2026-09-20:raw 地址匿名可取,会 302 到 `raw.giteeusercontent.com` 的签名地址);
      文件名的版本号来自 Makefile 的 `APP_VERSION`,由 `tools/release.sh` 第 5 步拷入;
- [ ] 本地测试仓库(`repo.json` + Sphaira 自定义商店源)能完成搜索→安装→更新→卸载;
- [ ] PR 里带上 `icon.png`(必需)与 `screen.png`/截图(推荐)。

PR 里该带的文件(其余都是构建期产物,按上游 README 排除):

```
packages/acnh-manager/pkgbuild.json
packages/acnh-manager/icon.png
packages/acnh-manager/screen.png            # 横幅,等设计稿
packages/acnh-manager/{status,install,done,uninstall}.png   # 截图源图
# 不带:screen1.png … screen4.png(构建期由 spinarak 生成)、info.json、manifest.install
```

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
