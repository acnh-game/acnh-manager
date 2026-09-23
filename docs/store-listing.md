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
| 图标 | `assets/icon.png`(256×256,与 NRO 内图标同源) | 必需;商店网格里要能一眼认出。**已知取舍**:列表与详情页共用这一个文件 —— 列表按磁贴宽度等比画(方形最饱满),详情页却把图标按原始像素画进 256×150 的槽并裁掉溢出(`ui/menus/appstore.cpp`:`icon_vec(968, …, 256, 150)`,DrawIcon 不缩放),所以方形图标在详情页上下各少约 53 px(2026-09-21 真机实测)。两个视图无法同时完美,已试过改成 256×150:详情页完整了,但列表里的主体明显变小(+两条补边),故维持 256×256 |
| 横幅 `screen.png` | `assets/screen.png`(848×208,由设计主图居中裁带得到;主图不入库) | 商店客户端把详情页顶部那块画成 **848×208**(Sphaira `ui/menus/appstore.cpp`:`banner_vec(70, …, 848.f, 208.f)`),缺图时回退成图标。**按比例裁切、不要缩放填满**;图里不写字(标题与说明由商店自己画)。换图:把新设计放成 `assets/screen-org.png`(仅本地需要,不入库)后跑 `tools/make-icons.py`,它会按 848:208 裁带生成 `screen.png`;没有主图时该步自动跳过 |
| 截图 | `assets/screenshots/{status,install,done,uninstall}.png`(真机、英文界面、1280×720 真 PNG,2026-09-23 重抓) | 商店按 `packages/<name>/screen<N>.png` 取图,所以 `tools/make-store-package.py` 会按 `SCREENSHOT_ORDER` 把它们声明成 `screenshot` 资产,并**额外**在包目录里生成 `screen1..4.png`(本地测试仓库与 CDN 布局用)。上游 README 明确说这类构建期文件可以不入 PR。**界面用英文**:商店目录每个字段只有一份文本,客户端不做翻译,英文图对中英用户都可用;顺序是 首页(已安装)/ 安装确认页 / 安装成功页 / 卸载确认页,真机抓取过程见 `device-acceptance.md` 末节 |

## 文案要点

- `description` 保持**英文一句话**(`Installer and manager for …`):它同时是商店搜索的索引字段
  (Sphaira 用 title / author / description 匹配关键词);
- `details` 与 `changelog` 写**中英双语、中文在前**(空行分隔),两边都保持简短:
  ① 做什么 + 可一键删除;② 只支持 ACNH 3.0.3、逐文件 SHA-256 校验;
- **为什么要写两遍**:商店目录每个字段只有一份文本,客户端原样显示 —— Sphaira 的 `_i18n` 只作用于它自己的
  界面标签(`version:` / `updated:` / `category:` …)与分类名,不会翻译条目文案(官方客户端 hb-appstore 同理)。
  实测官方仓库 497 个包里 `description` 含中日韩文字的 **0** 个、`details` 只有 **2** 个,所以双语是同时照顾
  中英文用户的唯一办法;
- 避免在英文文案里使用 "cheat":这是安装器,不是作弊包;中文文案同理。

## 上架检查清单

- [ ] `packaging/listing.json` 的中英文案与当前功能一致(尤其"当前支持范围");
- [x] `assets/icon.png` 与 `assets/screen.png`(横幅)都是最新设计稿;
- [x] 截图已就位(`assets/screenshots/`,真机四张);
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
packages/acnh-manager/screen.png            # 横幅
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

## 本地商店真机验收(Sphaira,菜单名取自其源码)

1. Mac 上把仓库目录用 HTTP 提供出去(和 Switch 同一个局域网):

   ```bash
   cd src/acnh-manager/build/scratch/store
   python3 -m http.server 8080 --bind 0.0.0.0
   # 先在 Mac 上自证:curl -sI http://<Mac 的局域网 IP>:8080/repo.json 应返回 200
   ```

2. Switch:相册 → Sphaira → **商店** → 按 `X` 打开选项 → 选 **商店来源**
   (`Store Source`)→ 在 **选择商店来源** 列表里选 **添加自定义商店...**;
3. 键盘输入名称(随意,例如 `ACNH-Test`),再输入 URL:**`http://<Mac 的局域网 IP>:8080`**
   —— Sphaira 会自己补上 `/repo.json`(`ui/menus/appstore.cpp` 里 `suffix{"/repo.json"}`),
   所以你写不写 `/repo.json` 都行;
4. 它会切到这个商店并加载:应能看到 `ACNH-Manager`(Tools,1.0.0),详情页顶部是横幅、
   下方是四张截图 —— 这一眼就把 `screen.png` 的尺寸/比例与截图顺序一起验了;
5. **安装**:按 `A` → 完成后卡上应出现 `/switch/ACNH-Manager/acnh-manager.nro`
   (商店包只包含这一个文件;agent 的三件套是 App 自己装的,不受影响);
6. **更新**:把本地包换成更高版本(`python3 tools/make-store-package.py --version 1.0.1`),
   回商店刷新 → 应显示可更新 → 更新后版本号变化、卡上文件被替换;
7. **卸载**:在商店里卸载 → `acnh-manager.nro` 从卡上消失(其余目录与 `state.json` 不动);
8. 测完清掉:切到该商店 → `X` → **删除当前商店**。

卡上缓存位置(出问题先看这里):`/switch/appstore/.get/stores/<store id>/{repo.json,icons,banners,packages}`。
