#include "app.hpp"

#include <cstdio>
#include <string>

namespace acnh_manager::ui {
namespace {

/* 家族配色(与图标同源):青绿主色、奶油底、沙色点缀。 */
constexpr Color kBackground{0xF6, 0xF1, 0xE3, 0xFF};
constexpr Color kHeader{0x2F, 0xBF, 0xA8, 0xFF};
constexpr Color kCard{0xFF, 0xFD, 0xF7, 0xFF};
constexpr Color kBorder{0xD8, 0xCF, 0xB6, 0xFF};
constexpr Color kText{0x2B, 0x2B, 0x2B, 0xFF};
constexpr Color kSubtle{0x6B, 0x6B, 0x6B, 0xFF};
constexpr Color kGood{0x1B, 0x7F, 0x5A, 0xFF};
constexpr Color kWarn{0xB5, 0x5A, 0x1E, 0xFF};
constexpr Color kOnHeader{0xFF, 0xFF, 0xFF, 0xFF};

constexpr int kHeaderHeight = 72;
constexpr int kFooterHeight = 44;
constexpr int kMargin = 40;
constexpr int kCardGap = 18;

std::string FormatBytes(std::uint64_t size) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(size));
    return buf;
}

}  // namespace

bool App::Init(FsFileSystem &sd) {
    m_sd = &sd;
    if (!m_font.Init()) {
        return false;
    }
    if (R_FAILED(framebufferCreate(&m_fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888,
                                  2))) {
        return false;
    }
    framebufferMakeLinear(&m_fb);
    m_fb_ready = true;
    Collect();
    return true;
}

void App::Exit() {
    if (m_fb_ready) {
        framebufferClose(&m_fb);
        m_fb_ready = false;
    }
    m_font.Exit();
}

void App::Collect() {
    if (m_sd == nullptr) {
        return;
    }
    m_report = env::Collect(*m_sd);
    /* M1 阶段没有内置清单,门控固定落在 NoManifest。 */
    m_gate = install::Evaluate(nullptr, m_report.build);
}

void App::Run() {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u32 down = padGetButtonsDown(&pad);
        if ((down & HidNpadButton_B) != 0 || (down & HidNpadButton_Plus) != 0) {
            break;
        }
        if ((down & (HidNpadButton_L | HidNpadButton_R)) != 0) {
            m_page = m_page == Page::Status ? Page::Settings : Page::Status;
        }
        if ((down & HidNpadButton_A) != 0) {
            if (m_page == Page::Settings) {
                m_language = m_language == i18n::Language::ZhHans ? i18n::Language::English
                                                                  : i18n::Language::ZhHans;
            } else {
                Collect();
            }
        }
        Render();
    }
}

void App::Card(Surface surface, int x, int y, int w, int h, const char *title) {
    FillRect(surface, x, y, w, h, kCard);
    StrokeRect(surface, x, y, w, h, 2, kBorder);
    FillRect(surface, x, y, w, 4, kHeader);
    if (title != nullptr) {
        m_font.Draw(surface, x + 18, y + 14, kFontHeading, kText, title);
    }
}

void App::Field(Surface surface, int x, int y, const char *label, const std::string &value,
                Color value_color) {
    m_font.Draw(surface, x, y, kFontBody, kSubtle, label);
    m_font.Draw(surface, x + 240, y, kFontBody, value_color, value);
}

void App::Render() {
    if (!m_fb_ready) {
        return;
    }
    void *buffer = nullptr;
    u32 stride = 0;
    framebufferBegin(&m_fb, &stride);
    buffer = m_fb.buf;
    Surface surface;
    surface.pixels = static_cast<u32 *>(buffer);
    surface.width = static_cast<int>(m_fb.width_aligned);
    surface.height = static_cast<int>(m_fb.height_aligned);
    surface.stride = static_cast<int>(stride / 4);

    Fill(surface, kBackground);
    RenderHeader(surface);
    if (m_page == Page::Status) {
        RenderStatusPage(surface);
    } else {
        RenderSettingsPage(surface);
    }
    RenderFooter(surface);
    framebufferEnd(&m_fb);
}

void App::RenderHeader(Surface surface) {
    FillRect(surface, 0, 0, surface.width, kHeaderHeight, kHeader);
    m_font.Draw(surface, kMargin, 16, kFontTitle, kOnHeader, Tr(i18n::StringId::AppTitle));
    const char *tabs[2] = {Tr(i18n::StringId::TabStatus), Tr(i18n::StringId::TabSettings)};
    int x = surface.width - kMargin;
    for (int i = 1; i >= 0; --i) {
        const int width = m_font.Measure(tabs[i], kFontHeading);
        x -= width;
        const bool active = (i == 0) == (m_page == Page::Status);
        m_font.Draw(surface, x, 24, kFontHeading, active ? kOnHeader : MakeColor(0xD5, 0xF2, 0xEC),
                    tabs[i]);
        if (active) {
            FillRect(surface, x, kHeaderHeight - 6, width, 4, kOnHeader);
        }
        x -= 32;
    }
}

void App::RenderFooter(Surface surface) {
    const int y = surface.height - kFooterHeight;
    FillRect(surface, 0, y, surface.width, kFooterHeight, kCard);
    StrokeRect(surface, 0, y, surface.width, 2, 2, kBorder);
    const char *hint = Tr(m_page == Page::Status ? i18n::StringId::HintControlsStatus
                                                 : i18n::StringId::HintControlsSettings);
    m_font.Draw(surface, kMargin, y + 8, kFontSmall, kSubtle, hint);
}

void App::RenderStatusPage(Surface surface) {
    int y = kHeaderHeight + kMargin / 2;
    const int width = surface.width - kMargin * 2;

    /* 游戏卡片 */
    Card(surface, kMargin, y, width, 190, Tr(i18n::StringId::SectionGame));
    int row = y + 56;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelHos), m_report.hos_version, kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelAppletMode),
          m_report.applet_type == AppletType_Application ? "Application" : "LibraryApplet", kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelGameRunning),
          Tr(m_report.application_running ? i18n::StringId::ValuePresent
                                          : i18n::StringId::ValueAbsent),
          m_report.application_running ? kWarn : kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelGameVersion),
          std::to_string(m_report.build.version), kText);
    if (m_report.build.module_id_known) {
        row += 30;
        Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelModuleId),
              m_report.build.module_id, kGood);
    }
    y += 190 + kCardGap;

    /* 覆盖与安装现状 */
    Card(surface, kMargin, y, width, 150, Tr(i18n::StringId::SectionOverride));
    row = y + 56;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelOverrideConfig),
          m_report.advice.text, m_report.advice.never_applies ? kWarn : kGood);
    row += 34;
    std::string files;
    for (const auto &file : m_report.exefs) {
        if (!files.empty()) {
            files += ", ";
        }
        files += file.name + " (" + FormatBytes(file.size) + ")";
    }
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelExistingFiles),
          files.empty() ? Tr(i18n::StringId::ValueNone) : files, kText);
    if (m_report.legacy_cheat_present) {
        row += 30;
        Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelLegacyCheat),
              m_report.legacy_cheat_name, kWarn);
    }
    y += 150 + kCardGap;

    /* 清单与门控 */
    const int remaining = surface.height - kFooterHeight - kCardGap - y;
    Card(surface, kMargin, y, width, remaining, Tr(i18n::StringId::SectionManifest));
    row = y + 56;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelManifestState),
          Tr(i18n::StringId::ManifestNotEmbedded), kWarn);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelGameVersion),
          std::to_string(m_report.build.version), kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelContentId),
          m_report.build.content_id.empty() ? Tr(i18n::StringId::ValueNotAvailable)
                                            : m_report.build.content_id,
          m_report.build.content_id.empty() ? kWarn : kGood);
    if (!m_report.problems.empty()) {
        row += 30;
        Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelProblems), m_report.problems,
              kWarn);
    }
    if (m_report.legacy_cheat_present) {
        m_font.Draw(surface, kMargin + 24, surface.height - kFooterHeight - 40, kFontSmall, kWarn,
                    Tr(i18n::StringId::LegacyCheatWarning), width - 48);
    }
}

void App::RenderSettingsPage(Surface surface) {
    int y = kHeaderHeight + kMargin / 2;
    const int width = surface.width - kMargin * 2;
    Card(surface, kMargin, y, width, 150, Tr(i18n::StringId::TabSettings));
    int row = y + 60;
    m_font.Draw(surface, kMargin + 24, row, kFontBody, kSubtle, Tr(i18n::StringId::LanguageLabel));
    const bool zh = m_language == i18n::Language::ZhHans;
    m_font.Draw(surface, kMargin + 240, row, kFontBody, kText,
                Tr(zh ? i18n::StringId::LanguageChinese : i18n::StringId::LanguageEnglish));
    row += 40;
    m_font.Draw(surface, kMargin + 24, row, kFontSmall, kSubtle,
                Tr(i18n::StringId::RefreshHint));
    m_font.Draw(surface, kMargin + 24, row + 26, kFontSmall, kSubtle, Tr(i18n::StringId::ExitHint));
}

}  // namespace acnh_manager::ui
