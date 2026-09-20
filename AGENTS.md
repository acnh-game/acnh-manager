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
⑤ 改过文案表必须跑 `python3 tools/check-i18n.py`:它校验 `StringId` 枚举与 `strings.cpp` 的表**逐条同序**
   并打印条目总数(`make -C tests` 也会调用它)。

## 仓库托管(Gitee)

- 本仓库托管在 **Gitee**:`https://gitee.com/acnh-game/acnh-manager`(公开),remote 为
  `git@gitee.com:acnh-game/acnh-manager.git`(SSH)。`packaging/listing.json` 的 `url` 必须与它一致
  (商店条目读这里);仓库必须公开:官方商店要求源码可查,而且 raw 端点本来就只认公开仓库。
- **对外托管的四个地址都是 Gitee raw**:`https://gitee.com/acnh-game/acnh-manager/raw/main/<仓库内路径>`。
  raw 请求会 302 到
  `raw.giteeusercontent.com` 的带签名地址(App 的 libcurl 开了跟随跳转,匿名可取,已实测)。
- 本机与 Gitee 打交道用 **`gitee` CLI**(v0.3.0;`gitee auth status` 显示 gitee.com / leolovenet)。
  它要联网,**沙盒里必须 escalation**:
  - 查看:`gitee repo view`、`gitee release list`、`gitee issue list`;
  - 任意 API:`gitee api /repos/acnh-game/acnh-manager/releases`(带认证;`--search` 能查端点);
  - 发版页:`gitee release create --tag v<版本> -n "..." -b "..."`(**CLI 不能给发行版传附件**,
    所以 NRO 不走发行版资产,见下);
- **NRO 就是仓库里的一个文件**:`packaging/nro/acnh-manager-<版本>.nro`,由 `tools/release.sh`
  第 5 步拷进去,raw 地址就是商店包 update 资产与指南页指向的 permalink(带版本号是有意的:
  某个版本的 update 资产必须永远指向那个版本的 NRO)。
- 推送与打 tag 直接用 git:**普通 `git push` 就行**(SSH,本机 key 已注册):
  `git push`、`git tag v<版本> && git push origin v<版本>`。
- 工作区里别的项目仍在 GitHub(根 `AGENTS.md` 里 `gh` 那条规矩),两者不要混用;
  Gitee CLI 与 `gh` 是两套身份,别拿 `gh` 去碰本仓库的远端。

## 纪律

- 只读参考 `src/libnx`、`src/Atmosphere-src`、`src/acnh-agent`;不要在本仓库复制它们的源码。
- 内嵌 payload 只能来自 acnh-agent 通过门控的发布产物:**发布走 `tools/release.sh`**(容器内现场构建
  agent 发布版,agent 仓库只读、`dist/` 不被碰),由导入工具写入 `packaging/agent/<版本>/`(发布记录,
  原始文件名)与 `data/`(bin2s 构建输入,必须带 `.bin` 后缀)。不得手工拷入未校验的二进制;
  发布后跑 `tools/verify-release.py --nro acnh-manager.nro` 核对四处一致。
- 过程性材料(spike 日志、截图、构建输出)放已忽略的 `build/scratch/`,不进 `docs/`。
- 新增/改名/删除工具或文档时,同一次改动内更新 `docs/tools-guide.md` 与本文档导航。
- **退出路径不许改回"返回加载器"**:`source/main.cpp` 必须保留 `__nx_applet_exit_mode = 1`。
  hbl 整个相册会话只用一个进程,退回加载器会把 applet 的显示层留在进程里累积,数次启动/退出后
  相册直接黑屏(真机复现;机制与验证见 `docs/architecture.md` 第 10 节)。
- **触摸判定必须用"按下期间的最后坐标"**:`hidGetTouchScreenStates` 在松手那一帧没有坐标
  (count=0),拿它做拖拽判定会让每次点击都变成"滑动"而静默失效。规则只能在
  `source/ui/action.hpp` 的 `TapTracker` 里改,并由 `tests/host_tests.cpp` 固住。
- **画出来的按钮必须能点**:每个可见控件都要在同一次改动里进 `m_actions`(页眉标签页、页脚
  提示也算),且命中矩形与绘制几何来自同一个布局函数;只读页要清空动作表,否则会点中上一页
  遗留的按钮。真机踩过这两条(`docs/architecture.md` 第 7 节)。
- **开发通道只许留在开发机上**:`/switch/ACNH-Manager/dev-manifest.json` 与 `payload/` 是
  `tools/make-dev-manifest.py` 生成的调试素材,正式通道(内嵌发布)优先,它们只在"内嵌通道不可用
  且详情页显式允许"时才被读取。**发布包、商店包、指南站下发物、用户卡上都不得包含这两个**;
  开发卡上的这份必须与当前发布版一致(刷法:把 `packaging/agent/<版本>/` 的三个产物与
  `manifest.json` 推到 `payload/` 与 `dev-manifest.json`),避免回退时装到过期 payload。
- **交给 `fs*` 的缓冲必须自己盖住整个声明窗口**:`fs*` 按声明长度 `FS_MAX_PATH`(0x301)映射一段
  页对齐 IPC 窗口,所以 `[buf, buf + FS_MAX_PATH)` 必须落在**那块缓冲自己的字节里** —— 路径落在
  映射末尾 0x301 字节内就会得到 `0xD401`(`InvalidMemoryState`),看着像"卡或会话坏了",实际是
  缓冲位置。做法:路径先复制进 `util::FsPath`(`source/util/fs_path.hpp`);缓冲是数组时(局部变量
  或结构体成员,如 `Log::Sink::path`),数组本身也必须 ≥ `FS_MAX_PATH`,并用 `static_assert` 钉住
  尺寸。引擎、环境探测、主程序、日志与开发开关检查都照此办理(真机根因与追踪数据见
  `docs/architecture.md` 第 4.2 节);唯一例外是演示用的 `probe.cpp`,它故意把裸指针交给 `fs*`。
- **发布清单必须签名**:`tools/sign-manifest.py --key ~/.acnh/acnh-manager-signing-key.pem`
  (私钥放本地密钥目录 `~/.acnh/`,600,不入库,要备份)把根目录 `agent-manifest.json`
  签成 `agent-manifest.json.sig`(名字就是"清单文件名 + `.sig`",App 按这个规则去取),两个文件一起提交。
  私钥不入库,公钥 `data/agent_pubkey.bin` 入库并编进 NRO;漏签或换钥的后果见
  `docs/architecture.md` 第 9.2 节。
- **工作线程的栈必须页对齐**:`threadCreate` 对调用方给的栈要求 `0x1000` 对齐,否则直接返回
  `LibnxError_BadInput`(`0x1759`),而且只在真机显现(`source/net/update_task.cpp`)。

## 语言约定

- **源码性质的文件全英文**(注释也算):`source/`、`tests/`、`tools/`、`Makefile` 里的注释、日志、
  诊断与脚本输出一律英文 —— 改动别人的英文文件时不要混进中文。
- **例外是 i18n**:`source/i18n/strings.cpp` 的中文列是产品数据,允许保留(全仓库仅此一处)。
  文档类文件(`AGENTS.md`、`README.md`、`docs/**`)不受限制。
- 新增用户可见文案一律进 i18n 表(两列都填),不要在代码里直接写中文句子 —— 安装引擎与网络层
  用 `i18n::Text` / `i18n::Format` 取文案,语言由 `i18n::SetLanguage` 跟着界面语言走。

## 文案原则(界面用语)

界面分两层:**首页只讲"现在怎样 + 会发生什么",专业信息一律进详情页**。下面每条都来自真机试用
里被指出过的问题,改动文案时逐条对照。

### 首页(写给普通玩家)

1. **状态说事实,按钮说动作。** 状态行只陈述现状,不要写"重装一次即可"这类提示 —— 用户想修
   会自己按按钮。反例:*需要重新安装一次,重装即可*;正例:*现在安装的 agent 不是当前发布版本*。
2. **不出现内部概念。** 门控 / 清单 / payload / exefs / NRO / sha256 / state.json / 内容 id /
   ModuleId 都不许出现在首页;标签一旦需要用户先学一个概念就不合格。反例:*现有覆盖文件*、
   *把游戏目录里的文件换回正确版本*;正例:*已安装的文件*、*删除已安装的文件*。
3. **一行一句,能删就删。** 首页只保留一句状态 + 一行版本号。反例:*3 个文件已校验*(用户不关心
   装了几个文件);也不要保留"点屏幕或按对应按键"这类教学句 —— 每个控件自带键徽章就够了。
4. **只展示影响判断的信息。** 版本号、是否有新版、能不能装 —— 这些会影响用户决策;文件数量、
   校验方式、内部编号不影响,放到详情页。
5. **同号也要能区分。** 当两种状态可能长得一样(版本号相同、构建不同)时,带上短标识:
   状态行用 `版本号(哈希前缀)` 的形式,而且用**安装器判定用的那个哈希**,不要另造标识。
6. **同一个词只表示同一件事。** 卸载/删除、安装/重装不要混用;`agent` 一旦引入就固定用它。
7. **失败给原因和去处,不给堆栈。** 一句话说清失败,并指向详情页/日志;首页不贴原始报错。
8. **中英两列同等通俗。** 英文列不是"开发者英文":`Not available` 可以,`Gate blocked` 不行。

### 详情页(写给我们自己排查)

9. **专业信息是"下沉",不是"删除"。** 首页删掉的都要在详情页能找到:检测结果/原因、内容 id、
   内嵌 agent 版本与 commit、安装记录与哈希、运行模式、日志路径、旧金手指冲突、开发开关。
10. **首页的承诺必须在详情页兑现。** 首页写"原因写在详情里",详情页就必须真的有那一行 ——
    实测踩过:文案承诺了,页面里却没有。
11. **诊断原文保留原样。** `state.json`、`payload`、`sha256`、`CA bundle`、`libcurl`、`socket`
    这类词只在详情页与 `log.txt` 出现,是排查锚点,不要为了"通俗"把它们改掉。

### 与文案配套的视觉语义

- **颜色只表达状态**:青绿 = 可以操作(还没装,邀请用户装)、绿 = 一切正常、橙 = 需要注意
  (版本不支持/需要重装)、红 = 操作失败。颜色不承担别的含义。
- **按键徽章代替文字提示**:控件上画 Ⓐ/Ⓧ/Ⓨ 徽章,页脚只保留 `Ⓑ 退出/返回`;不写"按 A 确认"。

## 目录规划

- `source/`:应用本体。M0 的探针(`main.cpp`/`probe.*`)不是一次性代码——`probe.*` 在 M1 演进为
  环境检查模块,`main.cpp` 的控制台壳将来换成界面壳。
- `source/version.hpp`:运行中的版本号,由 Makefile 的 `APP_VERSION` 经 `-DACNH_APP_VERSION`
  注入(和构建戳同一条路);清单门控的 `app.minVersion` 校验、日志与文本界面都用它。
  **不要在别处再写死版本字符串** —— 漏改一处就会用错版本去校验清单。
- `source/manifest/`:清单解析与校验(纯逻辑,可主机测试);`source/install/`:门控判定、安装决策、
  写盘引擎与 `state.json`(判定部分同上),外加从发布端取 payload 的 `network_source.*`;
  `source/payload/`:内嵌发布通道(读 `data/` 里 bin2s 生成的符号,含验签用的公钥);
  `source/ui/`:界面层;`source/net/`:联网检查(`http.*` 唯一传输、`signature.*` 验签、
  `update.*` 取清单+验签+解析、`update_policy.hpp` 决定"这份远端清单能不能当安装来源"、
  `update_task.*` 工作线程)。
  界面层里这几块纯逻辑单独成头文件、由主机测试钉住:`ui/action.hpp`(`HitTest`/`MoveFocus`/`TapTracker`)、
  `ui/header_tabs.hpp`(页眉标签页几何:绘制与命中矩形共用)、`ui/home_state.hpp`(首页八态分类)、
  `ui/settings.hpp`(`settings.json` 的往返:界面语言必须活过重启与卸载)——
  改规则时先改头文件与 `tests/host_tests.cpp`。
- `data/`:内嵌发布清单与 payload 的 bin2s 构建输入(由导入工具生成,入库),外加
  `data/agent_pubkey.bin` —— 验签用的公钥(由 `tools/make-signing-key.py` 生成,入库,编进 NRO)。
- `packaging/agent/<版本>/`:发布记录(subsdk9 / main.npdm / acnh-agent.version / manifest.json),
  与 `data/` 同源,也是对外托管的四个文件(玩家侧靠 Gitee raw 直取;仓库根目录还有一份
  `agent-manifest.json` 是它的副本,给 App 的更新检查读);`packaging/agent-lock.json` 是发布锁。
  `packaging/nro/`:每次发布拷进来的 `acnh-manager-<版本>.nro`(对外下载就是它的 raw 地址)。
- `tools/`:开发与真机迭代工具(见 `docs/tools-guide.md`)。
- `tests/`:主机侧单元测试(`make -C tests`);`packaging/`:M4 放商店打包与 `pkgbuild.json`;
  `docs/device-acceptance.md`:M5 写真机验收记录。

## 文档导航

| 路径 | 内容/何时读 |
|---|---|
| `README.md` | 项目概览与当前支持范围 |
| `docs/architecture.md` | 架构、版本识别与门控、SD 布局、环境检查(改行为前必读) |
| `docs/tools-guide.md` | 本仓库工具的用法与登记 |
| `docs/release-process.md` | 发布流程:导入门控、商店打包、上架与 Gitee 托管地址(发布前必读) |
| `docs/store-listing.md` | 商店条目元数据与素材要求(改对外文案前) |
| `docs/device-acceptance.md` | 真机验收矩阵与基线环境(每次真机验收后更新) |
| `../../docs/acnh_manager_plan.md` | 产品定稿方案(跨仓库决策) |
