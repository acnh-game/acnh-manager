#pragma once

/* 界面:状态 / 安装确认 / 卸载 / 结果 / 设置 五个页面。
   M2 阶段即可真机干跑整条流程:清单来自 SD 上的开发用清单文件,payload 来自 payload 目录,
   干跑模式(默认开启)只校验不写盘。 */

#include <switch.h>

#include <string>

#include "env/detect.hpp"
#include "i18n/strings.hpp"
#include "install/engine.hpp"
#include "install/gate.hpp"
#include "manifest/manifest.hpp"
#include "net/update.hpp"
#include "ui/font.hpp"

namespace acnh_manager {
class Log; /* log.hpp */
}

namespace acnh_manager::ui {

#ifndef ACNH_BUILD_STAMP
#define ACNH_BUILD_STAMP "unknown"
#endif

/* 页脚显示的构建戳(由 tools/build.sh 注入)。 */
inline constexpr const char *kBuildStamp = ACNH_BUILD_STAMP;

class App {
public:
    /* 失败时把失败步骤与返回码写进 error(便于写日志与在控制台显示)。 */
    bool Init(acnh_manager::Log *log, FsFileSystem &sd, std::string *error);
    void Exit();
    /* 主循环:在状态页按 B 或 + 返回。 */
    void Run();

private:
    enum class Page { Status, Install, Uninstall, Result, Settings };

    void Collect();
    void RefreshPlan();
    void RunInstall();
    void RunUninstall();

    void Render();
    void RenderHeader(Surface surface);
    void RenderFooter(Surface surface);
    void RenderStatus(Surface surface);
    void RenderInstall(Surface surface);
    void RenderUninstall(Surface surface);
    void RenderResult(Surface surface);
    void RenderSettings(Surface surface);
    void Field(Surface surface, int x, int y, const char *label, const std::string &value,
               Color value_color);
    void Card(Surface surface, int x, int y, int w, int h, const char *title);
    const char *Tr(i18n::StringId id) const { return i18n::Text(id, m_language); }
    void Trace(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    acnh_manager::Log *m_log{nullptr};
    FsFileSystem *m_sd{nullptr};
    Font m_font{};
    Framebuffer m_fb{};
    bool m_fb_ready{false};
    Page m_page{Page::Status};
    i18n::Language m_language{i18n::Language::ZhHans};

    env::EnvironmentReport m_report{};
    manifest::Manifest m_manifest{};
    bool m_have_manifest{false};
    std::string m_manifest_error{};
    install::GateResult m_gate{};
    install::InstallPlan m_plan{};
    install::InstallState m_state{};
    bool m_have_state{false};
    std::string m_state_error{};

    bool m_dry_run{true};
    bool m_allow_dev_manifest{false};
    std::string m_payload_dir{"/switch/ACNH-Manager/payload"};
    std::string m_dev_manifest_path{"/switch/ACNH-Manager/dev-manifest.json"};

    std::string m_progress_step{};
    int m_progress_index{0};
    int m_progress_total{0};
    bool m_result_ok{false};
    std::string m_result_error{};
    int m_result_files{0};
    bool m_result_dry_run{false};
    std::string m_update_status{"(未检查)"};
    /* 首帧逐段写日志:崩溃时能定位到具体绘制阶段。 */
    int m_trace_frames{1};
    /* 首帧分段暂停(dev-pause 存在时):每画完一段等按 +,用于逐段定位崩溃。 */
    bool m_stage_pause{false};
    void WaitForPlus(const char *stage);
};

}  // namespace acnh_manager::ui
