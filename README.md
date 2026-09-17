# ACNH-Manager

`acnh-agent` 的官方安装渠道:一个 Switch 自制应用(NRO),把对应游戏构建的 agent 文件写入 SD 卡,
并给出安装状态、失败原因与一键卸载回滚。

## 当前支持范围

- **仅支持 Animal Crossing: New Horizons 3.0.3**(title id `01006F8002326000`,标题版本 `2228224`)。
- 名字里的 `nh` = Nintendo Homebrew,是本项目线的家族前缀;覆盖全部动森作品,但当前只有上文这一个
  构建被支持,其他构建会被明确拒绝。

## 它做什么

1. 识别控制台上到底装的是哪个构建:读应用版本(`ns`)与更新包的 Program 内容 id(`ncm`),再用
   `(title id, 版本号, 内容 id)` 在发布清单 `games[]` 中选中对应条目;游戏恰好在运行时还会用
   `dmnt:cht` 读 `main` 的 ModuleId 做字节级复核。
2. 校验并写入 `atmosphere/contents/01006F8002326000/exefs/{subsdk9, main.npdm, acnh-agent.version}`,
   记录安装状态;支持一键卸载回滚。
3. 内嵌发布清单与 payload,离线也能安装;联网检查目前是**设置页手动按 `+`**,"启动时静默检查
   + 一键更新"属于 M5 计划(断网时静默使用内置 payload 这一点已经成立)。

判据链、实测证据与 SD 布局见 `docs/architecture.md`(改行为前必读);产品侧决策见
`../../docs/acnh_manager_plan.md`。

## 构建与部署

```bash
# 构建(固定镜像 devkitpro/devkita64:20260219;Docker 需 escalation)
./tools/build.sh

# 开发期部署到 Switch(经 sys-agent 的 FTP,端口 6001)
python3 tools/deploy-nro.py
```

产物为 `acnh-manager.nro`;正式发布经官方 Homebrew App Store,打包与上架流程见
`../../docs/acnh_manager_plan.md`。

## 图标

设计主图是 `assets/icon-org.png`(1254×1254,包裹 + 向下箭头 + 海滩元素,与"森友物码册"共用青绿/奶油/沙色
家族配色,不含文字)。由它生成两个产物并提交:`assets/icon.jpg`(256×256 JPEG,给 NACP/hbmenu/相册显示)
与 `assets/icon.png`(256×256 PNG,给商店条目);换图标时替换主图后跑 `tools/make-icons.py`。
首版曾直接沿用微信小程序图标,上架前改为这枚专用设计稿以区分两个应用并提升小尺寸可读性。

## 许可

GPL-3.0(见 `LICENSE`)。
