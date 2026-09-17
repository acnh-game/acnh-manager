# 发布流程

目标:把一个**已验证的** acnh-agent 构建与安装器一起发布到官方 Homebrew App Store,并保持
指南站、商店包、NRO 内嵌 payload 三处版本一致。

## 1. 前置条件

- acnh-agent 仓库工作树干净,且用正式构建形式产出:`make switch SEMANTIC_HOOK=1`
  → `dist/version.json` 必须 `dirty=false`、`buildFlags=2`(只含语义钩子位);
- acnh-agent 的派生 NPDM 能通过 `tools/make-minimal-npdm.py --verify`;
- 本仓库主机测试通过:`make -C tests`;设备构建零警告:`./tools/build.sh`。

## 2. 导入发布产物(门控)

```bash
python3 tools/import-agent-release.py --dry-run      # 先看门控结论,不落盘
python3 tools/import-agent-release.py               # 通过后写锁与内嵌 payload
```

门控(任一不过即拒绝,退出码 2):`dirty=false`、`buildFlags=2`、NSO 哈希与 `version.json` 一致、
派生 NPDM 重放校验通过、profile 的 `(titleId, buildId)` 一致。落盘内容:

```
packaging/agent-lock.json          发布锁(版本/commit/开关/文件哈希/构建指纹)
source/payload/<agentVersion>/     内嵌 payload + manifest.json
```

**现状**:acnh-agent 目前没有正式发布构建(现有 `dist` 是 `buildFlags=7` 的开发构建),所以这一步
现在必然被拒绝——这是设计如此,不要用手工数据绕过它。

## 3. 打包商店材料

```bash
./tools/build.sh                          # 产出 acnh-manager.nro
python3 tools/make-store-package.py       # 产出 build/scratch/store/
```

产物与用途:

| 路径 | 用途 |
|---|---|
| `packages/acnh-manager/pkgbuild.json` | 提交给官方数据仓库 `forusers/switch-hbas-repo` 的元数据(资产指向 GitHub Release) |
| `packages/acnh-manager/icon.png`、`screen.png` | 商店图标与横幅 |
| `zips/acnh-manager.zip` | 包内容(`switch/ACNH-Manager/acnh-manager.nro` + `manifest.install` + `info.json`) |
| `repo.json` | **本地测试仓库**:与官方 CDN 同布局,可作为 Sphaira 自定义商店源验证"搜索→安装→更新→卸载" |

## 4. 上架官方商店

1. 在 GitHub 建公开仓库 `leolovenet/acnh-manager` 并推送;打 tag 并上传 Release 资产
   `acnh-manager.nro`(`pkgbuild.json` 里的 `url` 指向它);
2. 向 `forusers/switch-hbas-repo` 提 PR:新增 `packages/acnh-manager/{pkgbuild.json,icon.png,screen.png}`;
3. PR 阶段 CI 会构建出一个预览仓库并评论在 PR 里,先按评论自查;
4. 合并到 main 后 CI(spinarak)重建并部署到 `switch.cdn.fortheusers.org`,当天生效;
   客户端受 repo.json 缓存影响,可能需要等一会儿或重新打开商店。

官方商店对"写入 `/atmosphere/contents/<tid>/` 的包"已有先例(`UltimateTrainingModpack`、`SwitchCast`),
但审核有裁量权;被拒或排队期间,**指南站直链兜底**必须可用。

## 5. 指南站同步(另一任务)

指南站按 SD 卡目录结构托管 agent 文件与同一份清单:

```
https://lextuo.com/acnh-chat-code/guide/agent/<agentVersion>/{subsdk9,main.npdm,acnh-agent.version}
https://lextuo.com/acnh-chat-code/guide/agent-manifest.json
https://lextuo.com/acnh-chat-code/guide/acnh-manager/<appVersion>/acnh-manager.nro
```

App 的联网检查默认读 `agent-manifest.json`(见 `docs/architecture.md` 第 9 节);两处版本必须一致,
由 `packaging/agent-lock.json` 与清单内容比对确认。

## 6. 版本与记录

- 本仓库版本(`APP_VERSION`)与商店条目 `version` 必须一致;功能更新升 Minor、修复升 Patch、
  特大变更先问用户(工作区规则);
- 每次发布后把结论写进 `docs/device-acceptance.md`:安装了哪个 agent 版本、在什么构建上验过、
  失败用例的观察结果。

## 7. 尚未完成的部分(明确记录)

- **内嵌 payload / romfs**:`source/payload/` 与内嵌清单的读取流程要在拿到第一个正式发布构建后接上
  (Makefile 的 `ROMFS` 与 App 的清单加载顺序);当前 App 只读 SD 上的开发用清单;
- **CA bundle**:联网检查需要 `/switch/ACNH-Manager/ca.pem` 或内嵌 CA(M4 收尾时二选一);
- **启动即静默检查**:目前是设置页手动触发,改成启动时后台线程(M5)。
