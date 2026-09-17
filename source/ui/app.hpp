#pragma once

/* 界面骨架:framebuffer + FreeType 文本,状态页与设置页。
   M1 阶段只做只读展示与语言切换;安装/卸载页在 M2 接入同一套渲染。 */

#include <switch.h>

#include "env/detect.hpp"
#include "i18n/strings.hpp"
#include "install/gate.hpp"
#include "ui/font.hpp"

namespace acnh_manager::ui {

class App {
public:
    bool Init(FsFileSystem &sd);
    void Exit();
    /* 主循环:按 B 或 + 返回。 */
    void Run();

private:
    enum class Page { Status, Settings };

    void Collect();
    void Render();
    void RenderHeader(Surface surface);
    void RenderFooter(Surface surface);
    void RenderStatusPage(Surface surface);
    void RenderSettingsPage(Surface surface);
    /* 非 const:Font 的字形缓存会在绘制时写入。 */
    void Field(Surface surface, int x, int y, const char *label, const std::string &value,
               Color value_color);
    void Card(Surface surface, int x, int y, int w, int h, const char *title);
    const char *Tr(i18n::StringId id) const { return i18n::Text(id, m_language); }

    FsFileSystem *m_sd{nullptr};
    Font m_font{};
    Framebuffer m_fb{};
    bool m_fb_ready{false};
    Page m_page{Page::Status};
    i18n::Language m_language{i18n::Language::ZhHans};
    env::EnvironmentReport m_report{};
    install::GateResult m_gate{};
};

}  // namespace acnh_manager::ui
