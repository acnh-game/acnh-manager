#include "app.hpp"

#include <cstdio>
#include <string>

#include "manifest/manifest.hpp"

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

const char *AppletTypeName(int type) {
    switch (type) {
        case AppletType_Application: return "Application";
        case AppletType_SystemApplication: return "SystemApplication";
        case AppletType_LibraryApplet: return "LibraryApplet";
        default: return "?";
    }
}

const char *PlanActionName(install::PlanAction action) {
    switch (action) {
        case install::PlanAction::Blocked: return "blocked";
        case install::PlanAction::Install: return "install";
        case install::PlanAction::Repair: return "repair";
        case install::PlanAction::UpToDate: return "up-to-date";
    }
    return "?";
}

std::string FormatBytes(std::uint64_t size) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(size));
    return buf;
}

}  // namespace

bool App::Init(FsFileSystem &sd, std::string *error) {
    m_sd = &sd;
    if (!m_font.Init(error)) {
        return false;
    }
    const Result rc_create =
        framebufferCreate(&m_fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc_create)) {
        if (error != nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "framebufferCreate rc=0x%08X", rc_create);
            *error = buf;
        }
        m_font.Exit();
        return false;
    }
    const Result rc_linear = framebufferMakeLinear(&m_fb);
    if (R_FAILED(rc_linear)) {
        if (error != nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "framebufferMakeLinear rc=0x%08X", rc_linear);
            *error = buf;
        }
        framebufferClose(&m_fb);
        m_font.Exit();
        return false;
    }
    m_fb_ready = true;
    Collect();
    /* 立刻画一帧:把"framebuffer 能画"与"能收集数据"分开暴露。 */
    Render();
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

    m_have_state = false;
    m_state_error.clear();
    if (!install::ReadStateFile(*m_sd, &m_state, &m_have_state, &m_state_error)) {
        m_state_error = "state.json 无法解析: " + m_state_error;
    }

    m_have_manifest = false;
    m_manifest_error.clear();
    bool found = false;
    if (!install::ReadManifestFile(*m_sd, m_dev_manifest_path, "0.1.0", !m_allow_dev_manifest,
                                   &m_manifest, &found, &m_manifest_error)) {
        m_manifest_error = "清单无效: " + m_manifest_error;
    } else {
        m_have_manifest = found;
    }
    RefreshPlan();
}

void App::RefreshPlan() {
    if (m_have_manifest) {
        m_gate = install::Evaluate(&m_manifest, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, m_manifest.agent);
    } else {
        m_gate = install::Evaluate(nullptr, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, manifest::AgentInfo{});
    }
}

void App::RunInstall() {
    if (!m_have_manifest || m_plan.action == install::PlanAction::Blocked ||
        m_plan.game == nullptr) {
        m_result_ok = false;
        m_result_error = m_have_manifest ? m_plan.reason : Tr(i18n::StringId::ResultNoManifest);
        m_page = Page::Result;
        return;
    }
    install::SdFolderPayloadSource source(*m_sd, m_payload_dir);
    const auto result = install::Install(*m_sd, m_manifest, *m_plan.game, source, m_dry_run,
                                        [this](const install::Progress &progress) {
                                            m_progress_step = progress.step;
                                            m_progress_index = progress.index;
                                            m_progress_total = progress.total;
                                        });
    m_result_ok = result.ok;
    m_result_error = result.error;
    m_result_files = result.files_written;
    m_result_dry_run = result.dry_run;
    if (result.ok) {
        Collect();
    }
    m_page = Page::Result;
}

void App::RunUninstall() {
    const auto result = install::Uninstall(*m_sd, m_dry_run, [this](const install::Progress &p) {
        m_progress_step = p.step;
        m_progress_index = p.index;
        m_progress_total = p.total;
    });
    m_result_ok = result.ok;
    m_result_error = result.error;
    m_result_files = result.files_written;
    m_result_dry_run = result.dry_run;
    if (result.ok) {
        Collect();
    }
    m_page = Page::Result;
}

void App::Run() {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u32 down = padGetButtonsDown(&pad);
        if ((down & (HidNpadButton_L | HidNpadButton_R)) != 0) {
            m_page = m_page == Page::Settings ? Page::Status : Page::Settings;
        }
        if ((down & HidNpadButton_B) != 0) {
            if (m_page == Page::Status) {
                break;
            }
            m_page = Page::Status;
        }
        if ((down & HidNpadButton_Plus) != 0 && m_page == Page::Status) {
            break;
        }
        if ((down & HidNpadButton_A) != 0) {
            switch (m_page) {
                case Page::Status: m_page = Page::Install; break;
                case Page::Install: RunInstall(); break;
                case Page::Uninstall: RunUninstall(); break;
                case Page::Result: Collect(); m_page = Page::Status; break;
                case Page::Settings:
                    m_language = m_language == i18n::Language::ZhHans ? i18n::Language::English
                                                                     : i18n::Language::ZhHans;
                    break;
            }
        }
        if ((down & HidNpadButton_X) != 0 && m_page == Page::Status) {
            m_page = Page::Uninstall;
        }
        if ((down & HidNpadButton_Y) != 0 && m_page == Page::Settings) {
            m_allow_dev_manifest = !m_allow_dev_manifest;
            Collect();
        }
        if ((down & HidNpadButton_X) != 0 && m_page == Page::Settings) {
            m_dry_run = !m_dry_run;
        }
        /* 联网检查更新:按需触发(启动静默检查需要工作线程,M5 再挪到后台)。 */
        if ((down & HidNpadButton_Plus) != 0 && m_page == Page::Settings) {
            const auto check = net::CheckForUpdate(net::kDefaultManifestUrl, net::kDefaultCaPath);
            if (check.ok) {
                const auto parsed = manifest::Parse(check.manifest_text, "0.1.0");
                m_update_status = parsed.ok ? std::string("清单可用,agent ") + parsed.manifest.agent.version
                                            : std::string("清单无效: ") + parsed.error;
            } else {
                m_update_status = (check.skipped ? "跳过: " : "失败: ") + check.reason;
            }
        }
        Render();
    }
}

void App::Render() {
    if (!m_fb_ready) {
        return;
    }
    u32 stride = 0;
    /* framebufferMakeLinear 之后必须画在 framebufferBegin 返回的影子缓冲上,
       framebufferEnd 再把它拷进真正的 framebuffer。 */
    void *framebuffer = framebufferBegin(&m_fb, &stride);
    Surface surface;
    surface.pixels = static_cast<u32 *>(framebuffer != nullptr ? framebuffer : m_fb.buf);
    surface.width = static_cast<int>(m_fb.width_aligned);
    surface.height = static_cast<int>(m_fb.height_aligned);
    surface.stride = static_cast<int>(stride / 4);

    Fill(surface, kBackground);
    RenderHeader(surface);
    switch (m_page) {
        case Page::Status: RenderStatus(surface); break;
        case Page::Install: RenderInstall(surface); break;
        case Page::Uninstall: RenderUninstall(surface); break;
        case Page::Result: RenderResult(surface); break;
        case Page::Settings: RenderSettings(surface); break;
    }
    RenderFooter(surface);
    framebufferEnd(&m_fb);
}

void App::RenderHeader(Surface surface) {
    FillRect(surface, 0, 0, surface.width, kHeaderHeight, kHeader);
    m_font.Draw(surface, kMargin, 16, kFontTitle, kOnHeader, Tr(i18n::StringId::AppTitle));
    const i18n::StringId tab_ids[2] = {i18n::StringId::TabStatus, i18n::StringId::TabSettings};
    int x = surface.width - kMargin;
    for (int i = 1; i >= 0; --i) {
        const char *label = Tr(tab_ids[i]);
        const int width = m_font.Measure(label, kFontHeading);
        x -= width;
        const bool active = (i == 0) == (m_page != Page::Settings);
        m_font.Draw(surface, x, 24, kFontHeading,
                    active ? kOnHeader : MakeColor(0xD5, 0xF2, 0xEC), label);
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
    i18n::StringId hint = i18n::StringId::HintControlsStatus;
    if (m_page == Page::Settings) {
        hint = i18n::StringId::HintControlsSettings;
    } else if (m_page == Page::Install || m_page == Page::Uninstall) {
        hint = i18n::StringId::HintControlsConfirm;
    } else if (m_page == Page::Result) {
        hint = i18n::StringId::HintControlsResult;
    }
    m_font.Draw(surface, kMargin, y + 8, kFontSmall, kSubtle, Tr(hint));
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
    m_font.Draw(surface, x + 240, y, kFontBody, value_color, value, 760);
}

void App::RenderStatus(Surface surface) {
    int y = kHeaderHeight + kMargin / 2;
    const int width = surface.width - kMargin * 2;

    Card(surface, kMargin, y, width, 190, Tr(i18n::StringId::SectionGame));
    int row = y + 56;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelHos), m_report.hos_version, kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelAppletMode),
          AppletTypeName(m_report.applet_type), kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelGameRunning),
          Tr(m_report.application_running ? i18n::StringId::ValuePresent
                                          : i18n::StringId::ValueAbsent),
          m_report.application_running ? kWarn : kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelGameVersion),
          std::to_string(m_report.build.version), kText);
    y += 190 + kCardGap;

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

    const int remaining = surface.height - kFooterHeight - kCardGap - y;
    Card(surface, kMargin, y, width, remaining, Tr(i18n::StringId::SectionManifest));
    row = y + 56;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelManifestState),
          m_have_manifest ? Tr(i18n::StringId::ValueDevManifest)
                          : Tr(i18n::StringId::ManifestNotEmbedded),
          m_have_manifest ? kGood : kWarn);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelContentId),
          m_report.build.content_id.empty() ? Tr(i18n::StringId::ValueNotAvailable)
                                            : m_report.build.content_id,
          m_report.build.content_id.empty() ? kWarn : kGood);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelGateResult),
          std::string(m_gate.reason) + " [" + PlanActionName(m_plan.action) + "]",
          m_gate.status == install::GateStatus::Supported ? kGood : kWarn);
    if (!m_report.problems.empty() || !m_manifest_error.empty() || !m_state_error.empty()) {
        row += 30;
        std::string problems = m_report.problems;
        if (!m_manifest_error.empty()) {
            problems += (problems.empty() ? "" : "; ") + m_manifest_error;
        }
        if (!m_state_error.empty()) {
            problems += (problems.empty() ? "" : "; ") + m_state_error;
        }
        Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelProblems), problems, kWarn);
    }
}

void App::RenderInstall(Surface surface) {
    int y = kHeaderHeight + kMargin / 2;
    const int width = surface.width - kMargin * 2;
    Card(surface, kMargin, y, width, 150, Tr(i18n::StringId::TabConfirm));
    int row = y + 56;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelPlan),
          std::string(PlanActionName(m_plan.action)) + " — " + m_plan.reason, kText);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelDryRun),
          Tr(m_dry_run ? i18n::StringId::ValueDryRunOn : i18n::StringId::ValueDryRunOff),
          m_dry_run ? kGood : kWarn);
    row += 30;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelPayloadDir), m_payload_dir, kSubtle);
    y += 150 + kCardGap;

    const int list_height = surface.height - kFooterHeight - kCardGap - y;
    Card(surface, kMargin, y, width, list_height, Tr(i18n::StringId::LabelPlan));
    row = y + 52;
    if (m_plan.game != nullptr) {
        for (const auto &file : m_plan.game->files) {
            if (row > y + list_height - 30) {
                break;
            }
            m_font.Draw(surface, kMargin + 24, row, kFontSmall, kText, file.name + "  →  " + file.target,
                        700);
            char meta[128];
            std::snprintf(meta, sizeof(meta), "%s  sha256 %s", FormatBytes(file.size).c_str(),
                          file.sha256.substr(0, 16).c_str());
            m_font.Draw(surface, kMargin + 24, row + 18, kFontSmall, kSubtle, meta, 700);
            row += 44;
        }
    } else {
        m_font.Draw(surface, kMargin + 24, row, kFontBody, kWarn,
                    Tr(i18n::StringId::ResultNoManifest), width - 48);
    }
    m_font.Draw(surface, kMargin + 24, surface.height - kFooterHeight - 34, kFontBody,
                m_result_ok ? kGood : kSubtle, Tr(i18n::StringId::ConfirmInstall));
}

void App::RenderUninstall(Surface surface) {
    int y = kHeaderHeight + kMargin / 2;
    const int width = surface.width - kMargin * 2;
    Card(surface, kMargin, y, width, 150, Tr(i18n::StringId::TabUninstall));
    m_font.Draw(surface, kMargin + 24, y + 60, kFontBody, kText,
                Tr(i18n::StringId::UninstallIntro), width - 48);
    int row = y + 60 + 40;
    Field(surface, kMargin + 24, row, Tr(i18n::StringId::LabelDryRun),
          Tr(m_dry_run ? i18n::StringId::ValueDryRunOn : i18n::StringId::ValueDryRunOff),
          m_dry_run ? kGood : kWarn);
    y += 150 + kCardGap;

    const int list_height = surface.height - kFooterHeight - kCardGap - y;
    Card(surface, kMargin, y, width, list_height, Tr(i18n::StringId::LabelExistingFiles));
    row = y + 52;
    if (!m_have_state) {
        m_font.Draw(surface, kMargin + 24, row, kFontBody, kWarn, Tr(i18n::StringId::ValueNone),
                    width - 48);
    } else {
        for (const auto &file : m_state.files) {
            if (row > y + list_height - 30) {
                break;
            }
            m_font.Draw(surface, kMargin + 24, row, kFontSmall, kText, file.target, 700);
            char meta[128];
            std::snprintf(meta, sizeof(meta), "%s  sha256 %s", FormatBytes(file.size).c_str(),
                          file.sha256.substr(0, 16).c_str());
            m_font.Draw(surface, kMargin + 24, row + 18, kFontSmall, kSubtle, meta, 700);
            row += 44;
        }
    }
    m_font.Draw(surface, kMargin + 24, surface.height - kFooterHeight - 34, kFontBody, kSubtle,
                Tr(i18n::StringId::ConfirmUninstall));
}

void App::RenderResult(Surface surface) {
    int y = kHeaderHeight + kMargin / 2;
    const int width = surface.width - kMargin * 2;
    Card(surface, kMargin, y, width, 240, Tr(i18n::StringId::TabResult));
    int row = y + 70;
    m_font.Draw(surface, kMargin + 24, row, kFontTitle, m_result_ok ? kGood : kWarn,
                Tr(m_result_ok ? i18n::StringId::ResultSuccess : i18n::StringId::ResultFailure));
    row += 56;
    char summary[160];
    std::snprintf(summary, sizeof(summary), "files=%d dry_run=%s", m_result_files,
                  m_result_dry_run ? "yes" : "no");
    m_font.Draw(surface, kMargin + 24, row, kFontBody, kSubtle, summary);
    row += 36;
    if (!m_result_error.empty()) {
        m_font.Draw(surface, kMargin + 24, row, kFontBody, kWarn, m_result_error, width - 48);
    }
    if (m_progress_total > 0) {
        char progress[96];
        std::snprintf(progress, sizeof(progress), "last step: %s (%d/%d)", m_progress_step.c_str(),
                      m_progress_index, m_progress_total);
        m_font.Draw(surface, kMargin + 24, y + 190, kFontSmall, kSubtle, progress);
    }
}

void App::RenderSettings(Surface surface) {
    int y = kHeaderHeight + kMargin / 2;
    const int width = surface.width - kMargin * 2;
    Card(surface, kMargin, y, width, 220, Tr(i18n::StringId::TabSettings));
    int row = y + 60;
    m_font.Draw(surface, kMargin + 24, row, kFontBody, kSubtle, Tr(i18n::StringId::LanguageLabel));
    m_font.Draw(surface, kMargin + 240, row, kFontBody, kText,
                Tr(m_language == i18n::Language::ZhHans ? i18n::StringId::LanguageChinese
                                                        : i18n::StringId::LanguageEnglish));
    row += 36;
    m_font.Draw(surface, kMargin + 24, row, kFontBody, kSubtle, Tr(i18n::StringId::LabelDryRun));
    m_font.Draw(surface, kMargin + 240, row, kFontBody, m_dry_run ? kGood : kWarn,
                Tr(m_dry_run ? i18n::StringId::ValueDryRunOn : i18n::StringId::ValueDryRunOff));
    row += 36;
    m_font.Draw(surface, kMargin + 24, row, kFontBody, kSubtle, Tr(i18n::StringId::LabelPayloadDir));
    m_font.Draw(surface, kMargin + 240, row, kFontSmall, kText, m_payload_dir);
    row += 36;
    m_font.Draw(surface, kMargin + 24, row, kFontBody, kSubtle, Tr(i18n::StringId::LabelManifestSource));
    m_font.Draw(surface, kMargin + 240, row, kFontSmall, kText,
                m_dev_manifest_path + (m_allow_dev_manifest ? "  [dev allowed]" : ""));
    row += 36;
    m_font.Draw(surface, kMargin + 24, row, kFontBody, kSubtle, "联网检查(+)");
    m_font.Draw(surface, kMargin + 240, row, kFontSmall, kText, m_update_status, 700);
}

}  // namespace acnh_manager::ui
