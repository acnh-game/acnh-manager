#include "app.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <unistd.h> /* close(): a handed-over connection nobody will read */

#include "manifest/manifest.hpp"
#include "install/content_install.hpp"
#include "install/container.hpp"
#include "install/game_install.hpp"
#include "install/dev_stats.hpp"
#include "install/range_probe.hpp"
#include "install/source_local_file.hpp"
#include "install/network_source.hpp"
#include "install/nx_services.hpp"
#include "ui/footer_version.hpp"
#include "ui/icon_image.hpp"
#include "ui/storage_bars.hpp"
#include "net/http_page.hpp"
#include "payload/embedded.hpp"
#include "log.hpp"
#include "net/update_policy.hpp"
#include "version.hpp"
#include "util/fs_path.hpp"
#include "util/json.hpp"
#include "util/size_text.hpp"
#include "util/swkbd.hpp"

#ifndef ACNH_DEV_AIDS
#define ACNH_DEV_AIDS 0
#endif

/* The probes are development aids and are absent from a release build (DEV_AIDS=0 in the
   makefile); the two places that call them below are behind the same switch. */
#if ACNH_DEV_AIDS
#include "probe.hpp"
#endif

namespace acnh_manager::ui {

void App::Trace(const char *fmt, ...) {
    if (m_log == nullptr) {
        return;
    }
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf) - 24, fmt, ap);
    va_end(ap);
    /* Stamped like the server's and the upload source's lines: one clock across all three is what
       lets a log read afterwards say where a slow step really was (2026-10-16). */
    char stamped[sizeof(buf) + 24];
    std::snprintf(stamped, sizeof(stamped), "[t=%llums] %s",
                  static_cast<unsigned long long>(
                      armTicksToNs(armGetSystemTick()) / 1000000ull), buf);
    m_log->Line("%s", stamped);
    m_log->Sync();
}

namespace {

#if ACNH_DEV_AIDS
/* Development switches live as empty files on the card; the app only checks whether the file
   can be opened, never its contents. */
bool DevFlagPresent(FsFileSystem &sd, const char *path) {
    FsFile flag{};
    const util::FsPath arg(path);
    if (R_FAILED(fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &flag))) {
        return false;
    }
    fsFileClose(&flag);
    return true;
}

#endif

/* Family palette (same origin as the icon): teal accent, cream background, sand highlights. */
constexpr Color kBackground{0xF6, 0xF1, 0xE3, 0xFF};
constexpr Color kHeader{0x2F, 0xBF, 0xA8, 0xFF};
constexpr Color kCard{0xFF, 0xFD, 0xF7, 0xFF};
constexpr Color kBorder{0xD8, 0xCF, 0xB6, 0xFF};
constexpr Color kText{0x2B, 0x2B, 0x2B, 0xFF};
constexpr Color kSubtle{0x6B, 0x6B, 0x6B, 0xFF};
constexpr Color kGood{0x1B, 0x7F, 0x5A, 0xFF};
/* Warnings are amber, not red.  The old value sat at a hue of 24 degrees, which read as brick red
   on the console; kBad is the palette's only red, and it means "the action failed".  Amber is a
   hue near 40 degrees: kWarn carries text and the status dot, kWarnBright is the strip's border
   and mark -- decoration, so it can be lighter than a legible text colour. */
constexpr Color kWarn{0x9A, 0x62, 0x00, 0xFF};
constexpr Color kWarnBright{0xD2, 0x9A, 0x00, 0xFF};
/* Fill behind the home warning strip: pale yellow rather than peach, so the whole strip reads as
   a caution instead of a pinkish alert. */
constexpr Color kWarnTint{0xFB, 0xF1, 0xCE, 0xFF};
constexpr Color kOnHeader{0xFF, 0xFF, 0xFF, 0xFF};
constexpr Color kHeaderSubtle{0xD5, 0xF2, 0xEC, 0xFF};
constexpr Color kHeaderBadge{0x59, 0xCD, 0xB9, 0xFF};
/* Face of the selected storage block in the install page's header: darker than the header, so the
   block reads as chosen (the app's other selected faces are darker than their background too) and
   the bar's lighter track stays visible inside it.  It is a *state* as much as an affordance: drawn
   on every screen that shows the pair, including the ones where the choice can no longer be changed,
   because "which storage this is going to" is worth knowing there too. */
constexpr Color kHeaderPanel{0x25, 0x9B, 0x88, 0xFF};
/* The same disc with no key in it: the storage selector's badges on the screens where the choice is
   settled.  A dull grey reads as "nothing to press here" against both the header and the panel; the
   chosen storage keeps the badge's own light mint, so the pair still says which one this run is
   writing to -- the same colour that disc had when it carried its `L` / `R` letter. */
constexpr Color kHeaderBadgeOff{0x84, 0xA9, 0xA2, 0xFF};
constexpr Color kBad{0xC0, 0x39, 0x2B, 0xFF};
constexpr Color kCardDisabled{0xF0, 0xEC, 0xE0, 0xFF};
/* State colour for "nothing installed yet": purple, so it reads differently from the teal of
   the action button and from the green/orange/red of the other states. */
constexpr Color kUninstalled{0x8A, 0x62, 0xD4, 0xFF};

void SetResultError(const char *what, Result rc, std::string *error) {
    if (error != nullptr) {
        char text[112];
        std::snprintf(text, sizeof(text), "%s (0x%08x)", what, static_cast<unsigned>(rc));
        *error = text;
    }
}

/* Custom forwarder records are valid enough for NCM and the HOME Menu, but nsDeleteApplication-
   Completely rejects them on some consoles.  Sphaira's deletion path removes the same record in
   NCM's own order: contents, meta key, then the NS application entity. */
bool DeleteApplicationRecordViaNcm(std::uint64_t title_id, std::string *error) {
    NsApplicationContentMetaStatus statuses[16]{};
    s32 status_count = 0;
    Result rc = nsListApplicationContentMetaStatus(title_id, 0, statuses, 16, &status_count);
    if (R_FAILED(rc)) {
        SetResultError("listing application meta failed", rc, error);
        return false;
    }

    bool ok = true;
    for (s32 i = 0; i < status_count && ok; ++i) {
        const NsApplicationContentMetaStatus &status = statuses[i];
        if (status.storageID != NcmStorageId_SdCard &&
            status.storageID != NcmStorageId_BuiltInUser) {
            continue;
        }

        NcmContentStorage storage{};
        rc = ncmOpenContentStorage(&storage, static_cast<NcmStorageId>(status.storageID));
        if (R_FAILED(rc)) {
            SetResultError("opening content storage failed", rc, error);
            ok = false;
            break;
        }
        NcmContentMetaDatabase db{};
        rc = ncmOpenContentMetaDatabase(&db, static_cast<NcmStorageId>(status.storageID));
        if (R_FAILED(rc)) {
            ncmContentStorageClose(&storage);
            SetResultError("opening content meta database failed", rc, error);
            ok = false;
            break;
        }

        NcmContentMetaKey key{};
        s32 total = 0;
        s32 written = 0;
        const std::uint64_t app_id = status.application_id;
        rc = ncmContentMetaDatabaseList(
            &db, &total, &written, &key, 1,
            static_cast<NcmContentMetaType>(status.meta_type), app_id, app_id, app_id,
            NcmContentInstallType_Full);
        if (R_FAILED(rc) || total != 1 || written != 1) {
            ncmContentMetaDatabaseClose(&db);
            ncmContentStorageClose(&storage);
            if (R_FAILED(rc)) {
                SetResultError("locating content meta failed", rc, error);
            } else if (error != nullptr) {
                *error = "content meta key was not unique";
            }
            ok = false;
            break;
        }

        NcmContentInfo infos[64]{};
        s32 info_count = 0;
        rc = ncmContentMetaDatabaseListContentInfo(&db, &info_count, infos, 64, &key, 0);
        if (R_FAILED(rc)) {
            ncmContentMetaDatabaseClose(&db);
            ncmContentStorageClose(&storage);
            SetResultError("listing content files failed", rc, error);
            ok = false;
            break;
        }
        for (s32 content = 0; content < info_count; ++content) {
            bool present = false;
            rc = ncmContentStorageHas(&storage, &present, &infos[content].content_id);
            if (R_FAILED(rc)) {
                ok = false;
                SetResultError("querying content file failed", rc, error);
                break;
            }
            if (present) {
                rc = ncmContentStorageDelete(&storage, &infos[content].content_id);
                if (R_FAILED(rc)) {
                    ok = false;
                    SetResultError("deleting content file failed", rc, error);
                    break;
                }
            }
        }
        if (ok) {
            rc = ncmContentMetaDatabaseRemove(&db, &key);
            if (R_FAILED(rc)) {
                SetResultError("removing content meta failed", rc, error);
                ok = false;
            } else {
                rc = ncmContentMetaDatabaseCommit(&db);
                if (R_FAILED(rc)) {
                    SetResultError("saving content meta failed", rc, error);
                    ok = false;
                }
            }
        }
        ncmContentMetaDatabaseClose(&db);
        ncmContentStorageClose(&storage);
    }

    if (ok) {
        rc = nsDeleteApplicationEntity(title_id);
        if (R_FAILED(rc) && rc != 0x410) {
            SetResultError("deleting application record failed", rc, error);
            ok = false;
        }
    }
    return ok;
}

bool DeleteApplicationTitles(std::initializer_list<std::uint64_t> title_ids,
                             std::string *error) {
    Result rc = nsInitialize();
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            char text[80];
            std::snprintf(text, sizeof(text), "opening ns failed (0x%08x)",
                          static_cast<unsigned>(rc));
            *error = text;
        }
        return false;
    }
    bool ok = true;
    for (const std::uint64_t title_id : title_ids) {
        rc = nsDeleteApplicationCompletely(title_id);
        if (R_FAILED(rc) && rc != 0x410) { /* 0x410: already absent */
            const Result complete_rc = rc;
            if (!DeleteApplicationRecordViaNcm(title_id, error)) {
                if (error != nullptr) {
                    char first[96];
                    std::snprintf(first, sizeof(first),
                                  "deleting %016llx failed (0x%08x)",
                                  static_cast<unsigned long long>(title_id),
                                  static_cast<unsigned>(complete_rc));
                    *error = std::string(first) + "; fallback: " + *error;
                }
                ok = false;
            }
        }
    }
    nsExit();
    return ok;
}

/* Taller than a plain title bar because the header carries a subtitle: what this program is,
   for the people who just installed it. */
constexpr int kHeaderHeight = 96;
constexpr int kFooterHeight = 44;
constexpr int kMargin = 40;
constexpr int kCardGap = 18;
/* Card layout: header height, horizontal padding, distance from the body to the bottom
   border, label column width, row gap.  Card height is measured from the content (see
   RowsHeight); when a page does not fit, the row gap tightens first and clipping is the
   last resort. */
/* Title band.  It was 56 px; 48 frees 8 px per card, which is what lets the details page keep
   a comfortable bottom padding in English (where one row wraps to two lines). */
constexpr int kCardHeader = 48;
constexpr int kCardPadX = 24;
/* Breathing room under the last row: 12 px read as "the text is falling out of the card"
   once a card had no row gap left. */
constexpr int kCardPadBottom = 22;
/* Top inset used when a card has no title (see DrawCard). */
constexpr int kCardPadTop = 26;
/* Gap between the file list's card title and its first row. */
constexpr int kListTopGap = 14;
/* The label column has to fit the longest English label ("Existing override files", about
   255 px), otherwise it would overlap the value. */
constexpr int kLabelColumn = 280;
constexpr int kRowGap = 8;

/* Home-screen action ids: the primary control and the three secondary controls. */
constexpr int kActionPrimary = 0;     /* install / update / repair / retry / check again */
constexpr int kActionCheckUpdate = 1; /* check for updates */
constexpr int kActionUninstall = 2;   /* remove the agent */
constexpr int kActionBack = 3;        /* B: leave the confirmation pages */
constexpr int kActionTabStatus = 10;  /* L: the status page (also a touch target) */
constexpr int kActionTabDetails = 11; /* R: the details page (also a touch target) */
constexpr int kActionFooterBack = 12; /* the footer's "Ⓑ exit / back" hint */
constexpr int kActionLanguage = 13;   /* the details page's language row */
constexpr int kActionGuide = 14;      /* the header's "-" entry: the chat-code explainer */
constexpr int kActionGameInstall = 15; /* the header's "+" entry: the game installer (modal page) */
constexpr int kActionInstallLocal = 16;    /* the installer's "this console" card (Y) */
constexpr int kActionInstallDownload = 17; /* the installer's "download" card (X) */
constexpr int kActionInstallUpload = 18;   /* the installer's "upload" card (A) */
constexpr int kActionInstallPickRow = 19; /* a row of the installer's file list */
constexpr int kActionInstallStart = 20;   /* "start" on the confirm screen (A) */
constexpr int kActionInstallBack = 21;    /* the installer's footer "Ⓑ back" hint (same as the key) */
constexpr int kActionInstallUrl = 22;     /* the address screen's "type it on the console" (A) */
constexpr int kActionInstallRecheck = 23; /* the same screen, when there is no network: ask again */
constexpr int kActionInstallTargetSd = 24;   /* the header's microSD block (L, or a tap) */
constexpr int kActionInstallTargetNand = 25; /* the header's system block (R, or a tap) */
constexpr int kActionPendingResume = 26;   /* "continue" on the unfinished-install screen (A) */
constexpr int kActionPendingAbandon = 27;  /* "give up and clean up" on that screen (X) */
constexpr int kActionPendingPickNew = 28;  /* "pick something else" on that screen (Y) */
constexpr int kActionStartupRepair = 29;   /* the startup page's only action (A) */
constexpr int kActionForwarder = 30;       /* install/remove the desktop tile */

constexpr bool IsHomeFocusAction(int id) {
    return id == kActionPrimary || id == kActionCheckUpdate || id == kActionUninstall ||
           id == kActionForwarder;
}

/* Where each header entry's key badge sits (the layout needs the same answer for the drawn pill
   and for the touch target -- see RenderHeader).  Same order as the tabs there: + installer,
   Ⓛ status, details (no badge), and the ring's closing Ⓡ after the guide. */
constexpr acnh_manager::ui::TabBadge kHeaderTabBadgePos[acnh_manager::ui::kHeaderTabCount] = {
    acnh_manager::ui::TabBadge::Before, acnh_manager::ui::TabBadge::Before,
    acnh_manager::ui::TabBadge::None,   acnh_manager::ui::TabBadge::After};

/* Player settings (language today).  Separate from state.json because uninstall deletes that. */
constexpr const char *kSettingsPath = "/switch/ACNH-Manager/settings.json";
/* The install in flight, if any: written by the engine before its first byte, read here to offer
   "continue" (design section 10).  It survives an interrupted run on purpose. */
constexpr const char *kTaskPath = "/switch/ACNH-Manager/install-task.json";
constexpr const char *kChatCodeGuideUrl = "https://lextuo.com/acnh-chat-code/guide/";

/* Header tab layout lives in ui/header_tabs.hpp so the host tests can pin it; these two
   constants have to agree with it. */
static_assert(kMargin == ui::kHeaderMargin, "header tab layout assumes the page margin");
static_assert(kHeaderHeight == ui::kHeaderBarHeight, "header tab layout assumes the bar height");

/* Footer hint geometry: the drawn "Ⓑ exit / back" badge plus its label and the touch target
   share these numbers, so the hint is a real button on a touch screen. */
struct FooterHintLayout {
    int badge_x;
    int badge_y;
    int badge;
    int text_y;
    int label_x;
    Rect hit;
};

FooterHintLayout LayoutFooterHint(int surface_height, int line_height, int label_width) {
    FooterHintLayout layout{};
    const int y = surface_height - kFooterHeight;
    layout.badge = 30;
    layout.badge_x = kMargin;
    layout.badge_y = y + (kFooterHeight - layout.badge) / 2;
    layout.text_y = y + (kFooterHeight - line_height) / 2 + 4;
    layout.label_x = kMargin + layout.badge + 12;
    const int right = label_width > 0 ? layout.label_x + label_width
                                      : layout.badge_x + layout.badge;
    layout.hit = Rect{layout.badge_x - 8, y, right - layout.badge_x + 16, kFooterHeight};
    return layout;
}

/* Badge colour used on the confirmation buttons (the home screen's accent teal). */
inline Color dot_face() { return kHeader; }

/* Sizes for a human: the file list is the one place we show them, and "73986 B" is noise.
   Two significant digits are plenty when the exact value lives in state.json. */
std::string FormatSize(u64 bytes) {
    /* One formatter for every screen: a 19.6 MB update must read "19.6 MB" on the confirm card,
       the progress line and the result, not "0.0 GB" (2026-10-19). */
    return util::FormatSize(bytes);
}


/* One formatted line appended to the environment block.  The block is written in a single write
   (Log::Block), so the lines are assembled here instead of going through App::Trace: 60 separate
   lines cost 2.2 s of open/write/flush/close on this card, and that was two seconds of frozen UI
   right after the first frame.  The buffer is larger than App::Trace's 192 bytes, so a long line in
   this block is no longer cut mid-word. */
void AddBlockLine(std::string *out, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void AddBlockLine(std::string *out, const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    *out += line;
    *out += '\n';
}

/* Names for the environment block of log.txt (see App::TraceEnvironment): the console's applet
   kind, a meta's kind, and the gate's verdict, each as one short word a support reader knows. */
const char *AppletTypeName(int type) {
    switch (type) {
        case AppletType_Application: return "Application";
        case AppletType_SystemApplication: return "SystemApplication";
        case AppletType_LibraryApplet: return "LibraryApplet";
        case AppletType_SystemApplet: return "SystemApplet";
        case AppletType_OverlayApplet: return "OverlayApplet";
        default: return "?";
    }
}

const char *MetaTypeName(u8 type) {
    switch (type) {
        case 0x80: return "Application";
        case 0x81: return "Patch";
        case 0x82: return "AddOnContent";
        case 0x83: return "Delta";
        default: return "?";
    }
}

const char *GateStatusName(install::GateStatus status) {
    switch (status) {
        case install::GateStatus::Supported: return "supported";
        case install::GateStatus::NoManifest: return "no-manifest";
        case install::GateStatus::TitleMissing: return "title-missing";
        case install::GateStatus::TitleNotSupported: return "title-not-supported";
        case install::GateStatus::VersionNotSupported: return "version-not-supported";
        case install::GateStatus::ContentIdMissing: return "content-id-missing";
        case install::GateStatus::ContentIdMismatch: return "content-id-mismatch";
        case install::GateStatus::BuildIdMismatch: return "build-id-mismatch";
    }
    return "?";
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

std::uint64_t ReadSystemLanguageCode(bool *known) {
    *known = false;
    if (R_FAILED(setInitialize())) {
        return 0;
    }
    std::uint64_t code = 0;
    const Result rc = setGetSystemLanguage(&code);
    setExit();
    if (R_FAILED(rc)) {
        return 0;
    }
    *known = true;
    return code;
}

bool FileIsMissing(FsFileSystem &sd, const char *path) {
    FsFile file{};
    const util::FsPath arg(path);
    const Result rc = fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file);
    if (R_SUCCEEDED(rc)) {
        fsFileClose(&file);
        return false;
    }
    return rc == 0x00000202; /* PathNotFound */
}


}  // namespace

bool App::Init(acnh_manager::Log *log, FsFileSystem &sd, std::string *error) {
    m_log = log;
    m_sd = &sd;
    if (m_startup_page) {
        m_page = Page::StartupCheck;
    }
    m_update_task.SetLog(log);
    /* The chat-code explainer reads its code once; empty when the build carries none. */
    m_guide_qr = ui::QrMatrix::FromEmbedded();
    /* High error correction leaves room for the small centered brand mark. */
    m_guide_web_qr = util::EncodeQr(kChatCodeGuideUrl, 3);
    LoadSettings();
    RefreshFooterText();
    Trace("ui: font init ...");
    if (!m_font.Init(log, error)) {
        return false;
    }
    Trace("ui: font init ok");
    if (m_log != nullptr) {
        install::dev::LogStartup(*m_log, "fonts ready");
    }
    /* Same as EdiZon-SE: always ask for two buffers. */
    const u32 num_fbs = 2;
    Trace("ui: framebufferCreate ... (applet=%d num_fbs=%u)",
          static_cast<int>(appletGetAppletType()), num_fbs);
    const Result rc_create =
        framebufferCreate(&m_fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, num_fbs);
    if (R_FAILED(rc_create)) {
        Trace("ui: framebufferCreate rc=0x%08X", rc_create);
        if (error != nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "framebufferCreate rc=0x%08X", rc_create);
            *error = buf;
        }
        m_font.Exit();
        return false;
    }
    Trace("ui: framebufferCreate ok");
    Trace("ui: framebufferMakeLinear ...");
    const Result rc_linear = framebufferMakeLinear(&m_fb);
    if (R_FAILED(rc_linear)) {
        Trace("ui: framebufferMakeLinear rc=0x%08X", rc_linear);
        if (error != nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "framebufferMakeLinear rc=0x%08X", rc_linear);
            *error = buf;
        }
        framebufferClose(&m_fb);
        m_font.Exit();
        return false;
    }
    Trace("ui: framebufferMakeLinear ok (buf=%p linear=%p)", m_fb.buf, m_fb.buf_linear);
    m_fb_ready = true;
    if (m_log != nullptr) {
        install::dev::LogStartup(*m_log, "framebuffer ready");
    }
    /* The startup self-check page needs the framebuffer and the fonts, not the game environment:
       collecting it and starting the update check would only delay the one repair the player has
       to make.  The page exits (or restarts into the standard copy) without ever showing Home. */
    if (!m_startup_page) {
        Trace("ui: collecting environment ...");
        Collect();
        /* The install an interruption left behind is part of the state of this console, so the details
           page can answer "where did those gigabytes go?" from the first frame on. */
        RefreshPendingTask();
        if (m_log != nullptr) {
            install::dev::LogStartup(*m_log, "environment + plan ready");
        }
        /* One silent look at the release host per launch: a newer agent shows up on the home screen
           on its own, and a failure only lands in the log/details. */
        StartUpdateCheck(/*silent=*/true);
        Trace("ui: environment collected (exefs=%zu, manifest=%d)", m_report.exefs.size(),
              m_have_manifest ? 1 : 0);
        /* Crash leftovers: an interrupted two-phase install can leave `<target>.acnh-tmp` /
           `<target>.acnh-old` siblings behind.  Clean them at startup so the directory never
           accumulates junk, and say so in the log. */
        if (m_sd != nullptr) {
            /* The manifest is the only source for where our files live; without it there is
               nothing we could have written, so there is nothing to clean either. */
            std::string dir;
            if (m_plan.game != nullptr && !m_plan.game->files.empty()) {
                const std::string &target = m_plan.game->files.front().target;
                const std::size_t slash = target.find_last_of('/');
                if (slash != std::string::npos) {
                    dir = target.substr(0, slash);
                }
            }
            if (!dir.empty()) {
                std::vector<std::string> leftovers;
                const int cleaned = install::CleanLeftovers(*m_sd, dir, &leftovers);
                /* Log whenever anything was *found*, including leftovers that refused to be removed:
                   a stuck temp file is the difference between a working and a failing install, and
                   this line is where it shows up. */
                if (!leftovers.empty() && m_log != nullptr) {
                    std::string list;
                    for (const auto &name : leftovers) {
                        if (!list.empty()) {
                            list += ", ";
                        }
                        list += name;
                    }
                    m_log->Line("cleanup: removed %d leftover file(s): %s", cleaned, list.c_str());
                }
            }
        }
    }
#if ACNH_DEV_AIDS
    /* Debug switch: while /switch/ACNH-Manager/dev-pause exists, stop before the first frame
       and wait for +, so sys-agent can read the memory layout of a live process (see
       docs/device-acceptance.md). */
    {
        FsFile flag{};
        /* Even a literal path goes through util::FsPath: fs declares FS_MAX_PATH bytes for it,
           and the rule is easier to keep than to remember the exceptions to. */
        const util::FsPath pause_flag("/switch/ACNH-Manager/dev-pause");
        const bool pause_before_first_frame =
            m_sd != nullptr &&
            R_SUCCEEDED(fsFsOpenFile(m_sd, pause_flag.c_str(), FsOpenMode_Read, &flag));
        if (pause_before_first_frame) {
            fsFileClose(&flag);
            Trace("ui: paused before first frame (press + to continue)");
            PadState pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&pad);
            while (appletMainLoop()) {
                padUpdate(&pad);
                if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
                    break;
                }
            }
            Trace("ui: resuming, drawing first frame");
            m_stage_pause = true;
        }
    }
#endif
    /* Paint one frame right away: that separates "the framebuffer works" from "collecting
       data works". */
    Trace("ui: first frame ...");
    Render();
    Trace("ui: first frame done");
    return true;
}

void App::SetStartupPlan(env::StartupPlan plan) {
    m_startup = std::move(plan);
    m_startup_page = m_startup.decision.show_page;
    m_startup_error = m_startup.startup_error;
    m_startup_repaired = m_startup.repaired;
}

void App::Exit() {
    m_update_task.Stop();
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
    /* The walk below writes ~48 lines, and each of them used to cost a full open/write/flush/close
       per sink -- about 1.5 s of the black screen.  A burst turns them into one write (see
       Log::BeginBurst); the update-check thread's lines ride along in the same buffer. */
    const LogBurst burst(m_log);
    m_report = env::Collect(*m_sd);

    m_have_state = false;
    m_state_error.clear();
    if (!install::ReadStateFile(*m_sd, &m_state, &m_have_state, &m_state_error)) {
        m_state_error = i18n::Format(i18n::StringId::StateParseFailed, m_state_error.c_str());
    }

    m_have_manifest = false;
    m_manifest_embedded = false;
    m_manifest_error.clear();
    /* Official channel: the release manifest embedded in the NRO (works offline). */
    if (payload::EmbeddedAvailable()) {
        const auto parsed = manifest::Parse(payload::EmbeddedManifestJson(), kAppVersion);
        if (parsed.ok) {
            m_manifest = parsed.manifest;
            m_have_manifest = true;
            m_manifest_embedded = true;
        } else {
            m_manifest_error =
                i18n::Format(i18n::StringId::ManifestEmbeddedInvalid, parsed.error.c_str());
        }
    }
    /* Development channel: the SD manifest is only read when the official channel is
       unavailable and the settings page explicitly allows dev manifests. */
    if (!m_have_manifest) {
        bool found = false;
        std::string dev_error;
        if (!install::ReadManifestFile(*m_sd, m_dev_manifest_path, kAppVersion, !m_allow_dev_manifest,
                                       &m_manifest, &found, &dev_error)) {
            dev_error = i18n::Format(i18n::StringId::ManifestInvalid, dev_error.c_str());
        }
        if (found) {
            m_have_manifest = true;
        } else if (!dev_error.empty()) {
            m_manifest_error =
                m_manifest_error.empty() ? dev_error : m_manifest_error + "; " + dev_error;
        }
    }
    /* The UI only has room for a short hint: the full diagnostic (including the raw manifest
       validation text) always lands in log.txt. */
    if (!m_state_error.empty()) {
        Trace("collect: state error: %s", m_state_error.c_str());
    }
    if (!m_manifest_error.empty()) {
        Trace("collect: manifest error: %s", m_manifest_error.c_str());
    }
    if (!m_report.problems.empty()) {
        Trace("collect: problems: %s", m_report.problems.c_str());
    }
    for (const std::string &note : m_report.notes) {
        Trace("collect: %s", note.c_str());
    }
    RefreshPlan();
    RefreshForwarderTile();
    UpdateHomeState();
    Trace("collect: gate=%s plan=%s", m_gate.reason.c_str(), PlanActionName(m_plan.action));
}

/* The environment block of log.txt, written off the collection that just ran.  Same lines the
   launcher used to write from its own collection -- one pass, one truth, and the gate line here is
   the gate the screens used rather than a second evaluation of the embedded manifest. */
void App::TraceEnvironment() {
    if (m_log == nullptr) {
        return;
    }
    const env::EnvironmentReport &report = m_report;
    std::string block;
    AddBlockLine(&block, "== ACNH-Manager %s ==", kAppVersion);
    AddBlockLine(&block, "HOS %s, applet=%s", report.hos_version.c_str(),
                 AppletTypeName(report.applet_type));
    AddBlockLine(&block, "game running=%d, patch storage=%u", report.application_running ? 1 : 0,
                 report.patch_found ? report.patch_storage : 0xFF);
    for (const auto &meta : report.metas) {
        AddBlockLine(&block, "  meta type=%s(%#04x) storage=%u version=%u(%#x)",
                     MetaTypeName(meta.meta_type), meta.meta_type, meta.storage, meta.version,
                     meta.version);
    }
    AddBlockLine(&block, "detected build: title=%s version=%u contentId=%s",
                 report.build.title_id.c_str(), report.build.version,
                 report.build.content_id.empty() ? "(unavailable)"
                                                 : report.build.content_id.c_str());
    AddBlockLine(&block, "game version: %s (NACP display version, from nsGetApplicationControlData)",
                 report.game_display_version.empty() ? "(unavailable)"
                                                     : report.game_display_version.c_str());
    if (report.build.module_id_known) {
        AddBlockLine(&block, "  running main ModuleId=%s (dmnt:cht)",
                     report.build.module_id.c_str());
    }
    AddBlockLine(&block, "override advice: %s", report.advice.text.c_str());
    AddBlockLine(&block, "exefs override files: %zu", report.exefs.size());
    for (const auto &file : report.exefs) {
        AddBlockLine(&block, "  %s (%llu B)", file.name.c_str(),
                     static_cast<unsigned long long>(file.size));
    }
    if (report.legacy_cheat_present) {
        AddBlockLine(&block, "legacy cheat present: %s entry=\"%s\" (same chat path as the agent)",
                     report.legacy_cheat_path.c_str(),
                     report.legacy_cheat_entry.empty() ? "(file unreadable)"
                                                       : report.legacy_cheat_entry.c_str());
    }
    if (!report.problems.empty()) {
        AddBlockLine(&block, "problems: %s", report.problems.c_str());
    }
    for (const std::string &note : report.notes) {
        AddBlockLine(&block, "%s", note.c_str());
    }
    /* Which manifest this build installs from.  The embedded release channel is the one the
       interface calls "bundled with the app"; the SD channel only appears when the embedded one is
       missing and the details page allows it, and saying which one won is the difference between
       "my manifest is not being read" and "it is being read and rejected". */
    std::string manifest_line;
    if (m_manifest_embedded) {
        manifest_line = "embedded, agent " + m_manifest.agent.version + " (commit " +
                        m_manifest.agent.commit + ")";
    } else if (m_have_manifest) {
        manifest_line = "development channel (SD), agent " + m_manifest.agent.version;
    } else if (!m_manifest_error.empty()) {
        manifest_line = m_manifest_error;
    } else {
        manifest_line = "none embedded (this build carries no release import)";
    }
    AddBlockLine(&block, "gate: %s (%s)", GateStatusName(m_gate.status), m_gate.reason.c_str());
    AddBlockLine(&block, "manifest: %s", manifest_line.c_str());
    /* One write for the whole block: sixty lines of open/write/flush/close cost 2.2 s on this card,
       and those seconds were a frozen window right after the first frame (see Log::Block). */
    m_log->Block(block);
}

/* "0.11.0(c47d2b47)": the version alone cannot tell two builds of the same version apart, so
   the first bytes of the payload hash ride along.  Same hash the installer verifies. */
std::string App::BuildLabel(const std::string &version, const std::string &sha256) const {
    std::string out = version;
    if (!sha256.empty()) {
        out += "(" + sha256.substr(0, 8) + ")";
    }
    return out;
}

/* The installed game's version in the words the console itself uses.  The NACP display string
   comes first: it is what the home menu prints under "Ver.", and for a bare 1.0.0 copy it is the
   only human-readable version there is (that build's numeric title version is 0). */
std::string App::InstalledGameVersion() const {
    if (!m_report.game_display_version.empty()) {
        return m_report.game_display_version;
    }
    /* When the detected build matched a manifest entry, that entry describes the same build, so
       its display version is a fair fallback if the NACP could not be read. */
    if (m_gate.game != nullptr && !m_gate.game->display_version.empty()) {
        return m_gate.game->display_version;
    }
    if (m_report.build.version != 0) {
        return std::to_string(m_report.build.version);
    }
    return std::string(Tr(i18n::StringId::ValueNotAvailable));
}

/* What this build of the app can install, but only when it is a *different* version from the one
   on the console.  The gate keeps the manifest entry even when it refused, so the "not supported"
   wording can name a version instead of only saying "no".  Empty means "do not name a version
   here": either nothing lists this title, or the versions agree and the gate refused for another
   reason (a content id or build id mismatch) -- printing the same number on both sides would read
   like a contradiction. */
std::string App::SupportedGameVersionNote() const {
    const manifest::GameEntry *entry =
        m_gate.game != nullptr ? m_gate.game : m_gate.title_entry;
    if (entry == nullptr) {
        return std::string();
    }
    /* The gate compares numeric title versions, people read display strings: only name a version
       when both of those say the two sides differ, so the note can never contradict the headline
       (a content id or build id refusal happens at the same version, and a stale NACP would show
       the same string on both sides). */
    if (entry->version == m_report.build.version) {
        return std::string();
    }
    const std::string supported = !entry->display_version.empty()
                                      ? entry->display_version
                                      : std::to_string(entry->version);
    return supported == InstalledGameVersion() ? std::string() : supported;
}

std::string App::GateResultText() const {
    switch (m_gate.status) {
        case install::GateStatus::Supported:
            return i18n::Format(i18n::StringId::GateResultSupported,
                                InstalledGameVersion().c_str());
        case install::GateStatus::NoManifest:
            return Tr(i18n::StringId::GateResultNoManifest);
        case install::GateStatus::TitleMissing:
            return Tr(i18n::StringId::GateResultTitleMissing);
        case install::GateStatus::TitleNotSupported:
            return Tr(i18n::StringId::GateResultTitleNotSupported);
        case install::GateStatus::VersionNotSupported: {
            const manifest::GameEntry *entry =
                m_gate.title_entry != nullptr ? m_gate.title_entry : m_gate.game;
            const std::string supported =
                entry != nullptr && !entry->display_version.empty()
                    ? entry->display_version
                    : std::string(Tr(i18n::StringId::ValueNotAvailable));
            return i18n::Format(i18n::StringId::GateResultVersion,
                                InstalledGameVersion().c_str(), supported.c_str());
        }
        case install::GateStatus::ContentIdMissing:
            return Tr(i18n::StringId::GateResultContentMissing);
        case install::GateStatus::ContentIdMismatch:
            return Tr(i18n::StringId::GateResultContentMismatch);
        case install::GateStatus::BuildIdMismatch:
            return Tr(i18n::StringId::GateResultBuildMismatch);
    }
    return Tr(i18n::StringId::ValueNotAvailable);
}

/* Settings live in their own file because the install record is deleted by uninstall; the
   language has to outlive that.  A missing or unreadable file is not an error: the defaults
   stand and the reason goes to the log. */
void App::LoadSettings() {
    Settings settings;
    std::string text;
    bool found = false;
    std::string error;
    if (!install::ReadTextFile(*m_sd, kSettingsPath, &text, &found, &error)) {
        Trace("settings: read failed: %s", error.c_str());
        return;
    }
    const bool first_launch = !found && FileIsMissing(*m_sd, kSettingsPath);
    if (first_launch) {
        bool system_language_known = false;
        const std::uint64_t system_language = ReadSystemLanguageCode(&system_language_known);
        settings.language =
            InitialLanguageFromSystemLanguageCode(system_language, system_language_known);
        Trace("settings: no file; first launch language=%s (system code=%s)",
              settings.language == i18n::Language::English ? "en" : "zh-Hans",
              system_language_known ? "available" : "unavailable");
    } else if (!found) {
        Trace("settings: file could not be read (%s), keeping defaults",
              error.empty() ? "no reason given" : error.c_str());
    } else {
        std::string problem;
        if (!ParseSettings(text, &settings, &problem)) {
            Trace("settings: unusable (%s), keeping defaults", problem.c_str());
        } else {
            /* A file we could read but whose values we did not understand reports that here
               (ParseSettings keeps the value it had) -- the log should say why. */
            if (!problem.empty()) {
                Trace("settings: %s", problem.c_str());
            }
            Trace("settings: language=%s",
                  settings.language == i18n::Language::English ? "en" : "zh-Hans");
        }
    }
    m_language = settings.language;
    i18n::SetLanguage(m_language);
    /* The install target is a preference too: the page opens on whatever the player picked last
       time, and the header's blocks show it as selected. */
    m_install_target = settings.install_to_nand ? install::TargetStorage::Nand
                                               : install::TargetStorage::SdCard;
    Trace("settings: install target=%s", settings.install_to_nand ? "nand" : "sd");
    if (first_launch) {
        SaveSettings();
    }
}

void App::SaveSettings() {
    if (m_sd == nullptr) {
        return;
    }
    Settings settings;
    settings.language = m_language;
    settings.install_to_nand = m_install_target == install::TargetStorage::Nand;
    std::string error;
    if (!install::WriteTextFile(*m_sd, kSettingsPath, DumpSettings(settings), &error)) {
        Trace("settings: could not be saved: %s", error.c_str());
    }
}

/* Hash of our payload inside the install record (empty when nothing is recorded). */
std::string App::InstalledPayloadHash() const {
    for (const auto &file : m_state.files) {
        if (file.target.find("subsdk9") != std::string::npos) {
            return file.sha256;
        }
    }
    return std::string();
}

/* Hash of the payload the release manifest would install. */
std::string App::ManifestPayloadHash() const {
    if (m_plan.game == nullptr) {
        return std::string();
    }
    for (const auto &file : m_plan.game->files) {
        if (file.name == "subsdk9") {
            return file.sha256;
        }
    }
    return std::string();
}

void App::RefreshPlan() {
    const manifest::Manifest &active = ActiveManifest();
    if (m_have_manifest || m_have_update_manifest) {
        m_gate = install::Evaluate(&active, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, active.agent);
    } else {
        m_gate = install::Evaluate(nullptr, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, manifest::AgentInfo{});
    }
    VerifyRecordAgainstCard();
}

/* The record is what the installer *did*; the card is what *is*.  `Plan()` only compares the
   record against the manifest, so this is the step that notices someone deleted or renamed the
   game directory while the app was closed -- otherwise the home screen keeps saying "installed"
   (measured: renaming `atmosphere/contents/<title>/exefs` over FTP while the app was closed
   left the old verdict on screen). */
void App::VerifyRecordAgainstCard() {
    m_files_incomplete = false;
    if (!m_have_state || m_plan.action != install::PlanAction::UpToDate) {
        return; /* nothing recorded, or the plan already asks for an install / repair */
    }
    std::string reason;
    if (install::VerifyInstalledFiles(*m_sd, m_state, &reason)) {
        return;
    }
    m_files_incomplete = true;
    Trace("collect: the card does not match the record: %s", reason.c_str());
    /* Same action, honest reason: "repair" is what the button offers, and the home screen gives
       it the wording this case deserves (see HomeKind::Incomplete). */
    m_plan.action = install::PlanAction::Repair;
    m_plan.reason = reason;
}

/* Fold everything the app knows into the single home-screen state. */
void App::UpdateHomeState() {
    HomeInputs in;
    /* Presence is what the content table says, not the version number: a bare 1.0.0 copy is
       installed and carries title version 0, and asking "version != 0" here reported exactly
       that console as "the game was not found". */
    in.game_found = !m_report.build.title_id.empty();
    in.supported = m_gate.status == install::GateStatus::Supported;
    in.last_failed = m_last_failed;
    in.files_incomplete = m_files_incomplete;
    in.repair_needed = m_plan.action == install::PlanAction::Repair;
    /* "Install" with a record present is an update, not a fresh install: the home screen must
       not claim "nothing is installed yet" to someone who has an older agent. */
    in.fresh_install = m_plan.action == install::PlanAction::Install && !m_have_state;
    in.newer_agent = !NewerAgentVersion().empty();
    m_home_kind = Classify(in);
    if (m_trace_frames > 0) {
        Trace("home: kind=%d (found=%d supported=%d failed=%d repair=%d fresh=%d newer=%d)",
              static_cast<int>(m_home_kind), in.game_found ? 1 : 0, in.supported ? 1 : 0,
              in.last_failed ? 1 : 0, in.repair_needed ? 1 : 0, in.fresh_install ? 1 : 0,
              in.newer_agent ? 1 : 0);
    }
}

void App::ToggleLanguage() {
    m_language = m_language == i18n::Language::ZhHans ? i18n::Language::English
                                                     : i18n::Language::ZhHans;
    /* The install engine and the network layer build user-visible messages too, so they read
       the same process-wide language; and the collected report carries sentences built at
       Collect() time (the override advice), so re-collect to rebuild them. */
    i18n::SetLanguage(m_language);
    RefreshFooterText();
    /* Remember it: the language used to live only in memory, so it was back to Chinese after
       every restart (reported from the console).  A failure here is logged and nothing more --
       the switch itself already took effect. */
    SaveSettings();
    Collect();
}

/* Update check, on the worker thread: `StartUpdateCheck()` only asks for one (the worker does
   the network), and `PollUpdateCheck()` -- called once per frame -- moves a finished result into
   the UI.  The interface therefore stays alive while the request is in flight. */
void App::StartUpdateCheck(bool silent) {
    m_update_silent = silent;
    const std::string start_error = m_update_task.Start(net::kDefaultManifestUrl);
    m_update_running = m_update_task.Running();
    if (!start_error.empty()) {
        /* No worker means no check: report it like any other failure, so the button never looks
           like it simply ignored the press (the raw text lands in the log and the details page). */
        net::UpdateCheckResult failed;
        failed.outcome = net::UpdateOutcome::WorkerFailed;
        failed.detail = start_error;
        ApplyUpdateCheck(std::move(failed));
        return;
    }
    Trace("update check: started (url=%s)", net::kDefaultManifestUrl);
}

void App::PollUpdateCheck() {
    net::UpdateCheckResult result;
    if (m_update_task.Take(&result)) {
        m_update_running = false;
        ApplyUpdateCheck(std::move(result));
        return;
    }
    m_update_running = m_update_task.Running();
}

void App::ApplyUpdateCheck(net::UpdateCheckResult result) {
    m_update_have_result = true;
    m_update_outcome = result.outcome;
    m_update_http_code = result.http_code;
    m_update_detail = result.detail;
    m_update_remote_version.clear();
    m_update_newer_unsupported = false;
    m_have_update_manifest = false;
    m_update_manifest = manifest::Manifest{};
    if (result.outcome == net::UpdateOutcome::Ok) {
        m_update_remote_version = result.agent_version;
        /* What this console has: the record's version, or the version this build carries when
           nothing is installed yet. */
        const std::string installed = m_have_state ? m_state.agent_version : std::string();
        const std::string current =
            !installed.empty() ? installed : (m_have_manifest ? m_manifest.agent.version : installed);
        const auto parsed = manifest::Parse(result.manifest_text, kAppVersion);
        if (parsed.ok) {
            /* The rule lives in net/update_policy.hpp (pure, host-tested): a newer release is
               only adopted when the gate accepts *this* build. */
            const auto decision = net::DecideAdoption(parsed.manifest, m_report.build, current);
            if (decision.adopt) {
                m_update_manifest = parsed.manifest;
                m_have_update_manifest = true;
            }
            m_update_newer_unsupported = decision.unsupported;
        }
    }
    Trace("update check: %s", m_update_detail.c_str());
    RefreshPlan();
    UpdateHomeState();
}

/* The version a press of the primary button would install, when that is newer than what this
   console actually has.  Empty means "nothing newer to offer": the home screen then keeps its
   normal states (fresh install, repair, retry, up to date). */
std::string App::NewerAgentVersion() const {
    const std::string available = ActiveManifest().agent.version;
    const std::string installed = m_have_state ? m_state.agent_version : std::string();
    if (available.empty() || installed.empty()) {
        return {}; /* nothing to compare against: that is not an update story */
    }
    return manifest::CompareVersions(available, installed) > 0 ? available : std::string();
}

const manifest::Manifest &App::ActiveManifest() const {
    if (m_have_update_manifest) {
        return m_update_manifest;
    }
    return m_manifest;
}

/* The check button's second line: idle, in flight, or the outcome -- built here, at render
   time, so it is always in the current language. */
std::string App::UpdateSubtitle() const {
    if (m_update_running) {
        return std::string(Tr(i18n::StringId::SubUpdateChecking));
    }
    if (!m_update_have_result) {
        return std::string(Tr(i18n::StringId::SubCheckUpdate));
    }
    /* The startup check is silent about failure: a machine that is offline when the app opens
       should not greet the player with an error they did not ask for.  The details page and the
       log still carry the reason. */
    if (m_update_silent && m_update_outcome != net::UpdateOutcome::Ok) {
        return std::string(Tr(i18n::StringId::SubCheckUpdate));
    }
    switch (m_update_outcome) {
        case net::UpdateOutcome::Ok:
            /* A newer release that does not cover this build is the one case where the player
               needs to know *why* there is nothing to install: the release host moved on, this
               console's game build did not. */
            if (m_update_newer_unsupported) {
                return i18n::Format(i18n::StringId::SubUpdateNewerUnsupported,
                                    m_update_remote_version.c_str());
            }
            /* With nothing installed there is no "your version" to compare against, so the
               line states what the release host publishes instead of claiming "up to date". */
            if (!m_have_state) {
                return i18n::Format(i18n::StringId::SubUpdatePublished,
                                    m_update_remote_version.c_str());
            }
            return NewerAgentVersion().empty()
                       ? i18n::Format(i18n::StringId::SubUpdateUpToDate,
                                      m_update_remote_version.c_str())
                       : i18n::Format(i18n::StringId::SubUpdateNewer,
                                      m_update_remote_version.c_str());
        case net::UpdateOutcome::HttpStatus:
            return i18n::Format(
                i18n::StringId::SubUpdateFailed,
                i18n::Format(i18n::StringId::UpdateShortHttp, m_update_http_code).c_str());
        case net::UpdateOutcome::NoSignature:
        case net::UpdateOutcome::BadSignature:
        case net::UpdateOutcome::NoTrustAnchor:
            return i18n::Format(i18n::StringId::SubUpdateFailed,
                                Tr(i18n::StringId::UpdateShortSignature));
        case net::UpdateOutcome::InvalidManifest:
            return i18n::Format(i18n::StringId::SubUpdateFailed,
                                Tr(i18n::StringId::UpdateShortManifest));
        case net::UpdateOutcome::WorkerFailed:
            return i18n::Format(i18n::StringId::SubUpdateFailed,
                                Tr(i18n::StringId::UpdateShortInternal));
        case net::UpdateOutcome::NotHttps:
        case net::UpdateOutcome::Network:
            break;
    }
    return i18n::Format(i18n::StringId::SubUpdateFailed,
                        Tr(i18n::StringId::UpdateShortOffline));
}

/* Details page: the raw outcome, so "it failed" always comes with what actually happened. */
std::string App::UpdateDetailText() const {
    if (m_update_running) {
        return std::string(Tr(i18n::StringId::SubUpdateChecking));
    }
    if (!m_update_have_result) {
        return std::string(Tr(i18n::StringId::ValueNotChecked));
    }
    if (m_update_outcome == net::UpdateOutcome::Ok) {
        return m_update_newer_unsupported
                   ? i18n::Format(i18n::StringId::SubUpdateNewerUnsupported,
                                  m_update_remote_version.c_str())
                   : i18n::Format(i18n::StringId::UpdateCheckOk, m_update_remote_version.c_str());
    }
    return i18n::Format(i18n::StringId::UpdateCheckFailed, m_update_detail.c_str());
}

void App::RunInstall() {
    m_result_forwarder = false;
    if ((!m_have_manifest && !m_have_update_manifest) ||
        m_plan.action == install::PlanAction::Blocked || m_plan.game == nullptr) {
        m_result_ok = false;
        m_last_failed = true;
        /* Even when the engine is never reached, the result page has to say whether anything
           was written -- otherwise dry-run would show up as dry_run=no. */
        m_result_files = 0;
        m_result_error = (m_have_manifest || m_have_update_manifest)
                             ? m_plan.reason
                             : Tr(i18n::StringId::ResultNoManifest);
        m_page = Page::Result;
        return;
    }
    /* The install runs on this thread, so the interface cannot animate while it works -- but it
       can (and must) say what it is doing: one frame before the first byte moves, and one after
       every file.  Without this the player stares at the confirmation page for as long as the
       download takes, which reads as "the app hung" (that is the whole subject of
       docs/architecture.md 9.3). */
    BeginProgress(/*uninstall=*/false, static_cast<int>(m_plan.game->files.size()));
    std::string error;
    int files_written = 0;
    const bool ok = InstallAgentNow(&files_written, &error, [this](const install::Progress &progress) {
        UpdateProgress(progress);
    });
    m_result_ok = ok;
    m_last_failed = !ok;
    m_result_error = error;
    m_result_files = files_written;
    m_result_uninstall = false;
    m_result_forwarder = false;
    /* The reason only used to exist on screen: a failure reported by the user could not be
       looked up in log.txt afterwards, which is exactly what happened while running the
       installer fault-injection tests. */
    Trace("install: ok=%d files=%d%s%s", ok ? 1 : 0, files_written,
          error.empty() ? "" : " error=", error.c_str());
    /* Development switch: capture what the fs session can still do, in the state that just
       failed, instead of guessing from a later (healthy) run. */
#if ACNH_DEV_AIDS
    if (!ok && DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        probe::RunFsSessionProbe(*m_log, *m_sd, "after install failure");
    }
#endif
    if (ok) {
        /* The record now describes the version we just installed; the remote release is no
           longer "newer than what is installed", so the next Collect() drops it (and with it the
           update button). */
        m_have_update_manifest = false;
        Collect();
    }
    m_page = Page::Result;
}

bool App::InstallAgentNow(int *files_written, std::string *error,
                          const std::function<void(const install::Progress &)> &on_progress) {
    const manifest::Manifest &active = ActiveManifest();
    /* Payload matches the manifest's origin: the embedded manifest uses the embedded payload, a
       dev manifest uses the payload directory on the SD card, and a verified remote release is
       downloaded from the URLs it names (the engine verifies each file against the manifest
       before anything is written). */
    payload::EmbeddedPayloadSource embedded_source;
    install::SdFolderPayloadSource sd_source(*m_sd, m_payload_dir);
    install::NetworkPayloadSource network_source(active, *m_plan.game);
    install::PayloadSource *source = nullptr;
    if (m_have_update_manifest) {
        source = &network_source;
    } else if (m_manifest_embedded) {
        source = &embedded_source;
    } else {
        source = &sd_source;
    }
    /* Development switch: record every fs call the engine makes, so a failure can be read back
       with the pointer and memory state the kernel saw at that moment. */
#if ACNH_DEV_AIDS
    if (DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        install::SetFsTraceSink(m_log);
    }
#endif
    const auto result = install::Install(*m_sd, active, *m_plan.game, *source, /*dry_run=*/false,
                                        on_progress);
    install::SetFsTraceSink(nullptr);
    *files_written = result.files_written;
    *error = result.error;
    return result.ok;
}

void App::RunAgentFollowUp() {
    m_agent_follow_up = AgentFollowUp::NotRun;
    m_agent_follow_reason.clear();
    if (m_game_install == nullptr) {
        return;
    }
    const install::PackageInfo &package = m_game_install->Package();
    const install::InstallStatus &status = m_game_install->Status();
    if (status.stage != install::InstallStage::Done &&
        status.stage != install::InstallStage::Skipped) {
        return;
    }
    /* What the console ends up on: the version this run brings, or the one it already had when the
       package turned out to be pointless.  "This run" is the *newest* version-carrying meta of the
       container, not the base game's: a dump that carries a game and its update lands on the
       update's version, and that is the version the agent has to work with.  Only base games and
       updates bring a game version at all -- an add-on or a data patch changes none, and a version
       the manifest does not name is one the agent cannot work with, which the confirm page already
       said. */
    const std::uint32_t effective = status.stage == install::InstallStage::Skipped
                                        ? status.installed_version
                                        : status.effective_version;
    if (!install::MetaTypeCarriesGameVersion(package.cnmt.type) ||
        !AgentSupportedFor(package.cnmt.title_id, effective)) {
        return;
    }
    /* The console answers differently now, and the plan is what says whether there is anything to
       write: up to date means the agent for this version is already in place. */
    Collect();
    if (m_plan.action == install::PlanAction::UpToDate) {
        m_agent_follow_up = AgentFollowUp::AlreadyCurrent;
        Trace("install: agent follow-up: already up to date, nothing written");
        return;
    }
    if (m_plan.action == install::PlanAction::Blocked || m_plan.game == nullptr) {
        m_agent_follow_up = AgentFollowUp::Failed;
        m_agent_follow_reason = m_plan.reason.empty() ? "no plan for the agent" : m_plan.reason;
        m_result_ok = false;
        m_last_failed = true;
        m_result_error = m_agent_follow_reason;
        Trace("install: agent follow-up: blocked: %s", m_agent_follow_reason.c_str());
        Collect();
        return;
    }
    /* The card now shows this step: the package's own transfer is over. */
    m_install_follow_up = true;
    m_progress_step.clear();
    m_progress_index = 0;
    m_progress_total = static_cast<int>(m_plan.game->files.size());
    m_install_screen = InstallScreen::Progress;
    Render();
    int files_written = 0;
    std::string error;
    const bool ok = InstallAgentNow(&files_written, &error, [this](const install::Progress &p) {
        UpdateProgress(p);
    });
    m_install_follow_up = false;
    m_result_ok = ok;
    m_result_files = files_written;
    m_result_uninstall = false;
    m_result_forwarder = false;
    m_agent_follow_up = ok ? AgentFollowUp::Installed : AgentFollowUp::Failed;
    m_agent_follow_reason = ok ? std::string() : error;
    m_result_error = error;
    m_last_failed = !ok;
    if (ok) {
        /* The release we just wrote is the newest one there is, so the update button goes away. */
        m_have_update_manifest = false;
    }
    Trace("install: agent follow-up %s files=%d%s%s", ok ? "installed" : "failed", files_written,
          error.empty() ? "" : " error=", error.c_str());
    Collect();
}

void App::RunUninstall() {
#if ACNH_DEV_AIDS
    if (DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        install::SetFsTraceSink(m_log);
    }
#endif
    BeginProgress(/*uninstall=*/true, static_cast<int>(m_state.files.size()));
    const auto result = install::Uninstall(*m_sd, /*dry_run=*/false,
                                           [this](const install::Progress &p) {
                                               UpdateProgress(p);
                                           });
    install::SetFsTraceSink(nullptr);
    /* The result page words itself from these two flags; without this the page claimed
       "install succeeded" right after removing the files. */
    m_result_uninstall = true;
    m_result_forwarder = false;
    m_result_ok = result.ok;
    m_last_failed = !result.ok;
    m_result_error = result.error;
    m_result_files = result.files_written;
    Trace("uninstall: ok=%d files=%d%s%s", result.ok ? 1 : 0, result.files_written,
          result.error.empty() ? "" : " error=", result.error.c_str());
#if ACNH_DEV_AIDS
    if (!result.ok && DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        probe::RunFsSessionProbe(*m_log, *m_sd, "after uninstall failure");
    }
#endif
    if (result.ok) {
        Collect();
    }
    m_page = Page::Result;
}

/* Stick half of the input model: the same rows the d-pad walks, with a repeat so a long list can
   be scrolled without hammering the stick.  Dead zone first (a worn stick must not drift the
   focus), then an immediate first step, then a faster repeat while it stays pushed. */
void App::HandleStick(const PadState &pad) {
    constexpr int kDeadZone = 14000;   /* out of 32767 */
    constexpr u64 kFirstDelayMs = 320; /* before the first repeat */
    constexpr u64 kRepeatMs = 110;
    const HidAnalogStickState stick = padGetStickPos(&pad, 0);
    HomeMoveDirection move = HomeMoveDirection::None;
    if (m_page == Page::Home) {
        move = HomeDirectionFromStick(stick.x, stick.y, kDeadZone);
    } else if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Files) {
        if (stick.y > kDeadZone) {
            move = HomeMoveDirection::Previous;
        } else if (stick.y < -kDeadZone) {
            move = HomeMoveDirection::Next;
        }
    }
    if (move == HomeMoveDirection::None) {
        m_stick_next_tick = 0; /* centred: the next push steps immediately */
        return;
    }
    const u64 now = static_cast<u64>(armTicksToNs(armGetSystemTick()) / 1000000ULL);
    if (m_stick_next_tick != 0 && now < m_stick_next_tick) {
        return;
    }
    if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Files) {
        m_file_list.MoveFocus(move == HomeMoveDirection::Previous ? -1 : +1);
    } else if (m_page == Page::Home) {
        MoveHomeFocus(move == HomeMoveDirection::Previous ? -1 : +1);
    } else {
        return;
    }
    m_stick_next_tick = now + (m_stick_next_tick == 0 ? kFirstDelayMs : kRepeatMs);
}

/* Keyboard half of the input model.  Home always has one focused button: Up/Down move the focus
   and A activates it.  The details page uses spatial focus and A, and B means "back" (or exit on
   Home). */
void App::HandleKeys(u32 down) {
    /* While the engine is working, the progress page owns the screen and the thread: no key can
       leave it (the install is synchronous, so in practice nothing arrives here at all -- this
       guard is what keeps that true if the install ever moves to a worker). */
    if (m_page == Page::Progress) {
        return;
    }
    if (m_page == Page::StartupCheck) {
        /* One action, one way out.  No tabs, no L/R, no installer/guide shortcuts: this page is
           the precondition for everything else. */
        if ((down & HidNpadButton_B) != 0) {
            m_should_exit = true;
            return;
        }
        if ((down & HidNpadButton_A) != 0) {
            if (!m_startup_repaired &&
                m_startup.decision.action != env::StartupAction::ReportOnly) {
                RepairStartup();
            }
            return;
        }
        return;
    }
    /* L / R move through the pages the player reads, in the order the header draws them, wrapping
       at both ends (see SwitchPeerPage).  They are dead while the installer is up: that page is a
       mode whose header hides the tabs, and a key that silently left it moved the player out with
       the package in hand -- its placeholders and the open card file abandoned, with nothing
       owning them.  Everything here that carries a header entry is dead there for the same
       reason, which is why + and - below are guarded too. */
    if (m_page != Page::GameInstall) {
        if ((down & HidNpadButton_L) != 0) {
            SwitchPeerPage(-1);
            return;
        }
        if ((down & HidNpadButton_R) != 0) {
            SwitchPeerPage(+1);
            return;
        }
    }
    /* The installer page is the exception: it has its own stack of screens, and there B means "one
       step back" (see the Page::GameInstall case below, which is where that is decided).  Handling
       it here too is what turned "back" into "leave the installer" on every one of its screens --
       and *returning* here instead of falling through would leave B doing nothing at all. */
    if ((down & HidNpadButton_B) != 0 && m_page != Page::GameInstall) {
        if (m_page == Page::Home) {
            m_should_exit = true;
            return;
        }
        /* Leaving the guide or the installer goes back to whatever it was opened from;
           everywhere else B is "back to the status page". */
        if (m_page == Page::Guide) {
            m_page = m_guide_return;
        } else {
            m_page = Page::Home;
        }
        m_focus = FirstEnabled(m_actions);
        return;
    }
    if (m_page != Page::GameInstall) {
        if ((down & HidNpadButton_Plus) != 0) {
            /* + is the game installer, from any page: it is modal, so B is the only way back and
               the page hides the header entries while it is up. */
            OpenGameInstall();
            return;
        }
        if ((down & HidNpadButton_Minus) != 0) {
            /* - is the chat-code explainer, from any page: the first press opens it, the next one
               (or B) goes back.  It is also the ring's last stop, so it is a shortcut now rather
               than the only way in.  Exiting stays on B, which is where the footer points. */
            ToggleGuide();
            return;
        }
    }
    if (m_page == Page::Home) {
        const HomeMoveDirection move = HomeDirectionFromButtons(
            (down & HidNpadButton_Up) != 0, (down & HidNpadButton_Right) != 0,
            (down & HidNpadButton_Down) != 0, (down & HidNpadButton_Left) != 0);
        if (move == HomeMoveDirection::Previous) {
            MoveHomeFocus(-1);
        } else if (move == HomeMoveDirection::Next) {
            MoveHomeFocus(+1);
        } else if ((down & HidNpadButton_A) != 0) {
            if (m_focus >= 0) {
                ActivateAction(m_focus);
            }
        }
        return;
    }
    switch (m_page) {
        case Page::Details:
            if ((down & HidNpadButton_A) != 0) {
                /* The only switchable row today is the language (the same action a tap on the
                   row uses); the dev manifest toggle is a development aid and stays on Y. */
                ActivateAction(kActionLanguage);
            } else if ((down & HidNpadButton_Y) != 0) {
                m_allow_dev_manifest = !m_allow_dev_manifest;
                Collect();
            }
            break;
        case Page::GameInstall:
            /* One key map per screen: the picker moves with up/down and picks with A, the source
               screen uses Y/X/A for the three sources, and B (or the footer's Ⓑ hint, which is the
               same control) steps back one screen -- leaving the modal page from the source
               screen.  See InstallStepBack(). */
            if (m_install_screen == InstallScreen::Progress) {
                break; /* the engine owns the thread: no key does anything here */
            }
            /* L / R pick the storage this run writes to: the tabs are hidden on this page, so the
               "switch pages" meaning they have everywhere else would promise controls that are not
               on the screen (design section 8.1).  SD is the left block, the console's memory the
               right one, so the key on each side selects the block on that side. */
            if ((down & HidNpadButton_L) != 0) {
                SetInstallTarget(install::TargetStorage::SdCard);
                break;
            }
            if ((down & HidNpadButton_R) != 0) {
                SetInstallTarget(install::TargetStorage::Nand);
                break;
            }
            if ((down & HidNpadButton_B) != 0) {
                InstallStepBack();
                break;
            }
            if (m_install_screen == InstallScreen::Pending) {
                /* The unfinished install's own screen: the three entries are the only ways on. */
                if ((down & HidNpadButton_A) != 0) {
                    ActivateAction(kActionPendingResume);
                } else if ((down & HidNpadButton_X) != 0) {
                    ActivateAction(kActionPendingAbandon);
                } else if ((down & HidNpadButton_Y) != 0) {
                    ActivateAction(kActionPendingPickNew);
                }
                break;
            }
            if (m_install_screen == InstallScreen::Confirm) {
                if ((down & HidNpadButton_A) != 0) {
                    RunGameInstall();
                }
                break;
            }
            if (m_install_screen == InstallScreen::Url) {
                /* A opens the system keyboard -- or, when there is no network to download over,
                   asks again whether there is one now.  B (handled above) leaves the screen, and
                   from the source screen that leaves the installer. */
                if ((down & HidNpadButton_A) != 0) {
                    if (m_install_no_network) {
                        RefreshNetworkState();
                    } else {
                        StartDownload();
                    }
                }
                break;
            }
            if (m_install_screen == InstallScreen::Upload) {
                /* The upload screen has no key of its own: everything happens in the browser.  A
                   is only the "is there a network now" question, which is the one thing the
                   console can still answer while the page is unreachable. */
                if ((down & HidNpadButton_A) != 0 && m_install_no_network) {
                    RefreshNetworkState();
                }
                break;
            }
            if (m_install_screen == InstallScreen::Files) {
                if ((down & HidNpadButton_Y) != 0) {
                    LeaveDirectory();
                } else if ((down & HidNpadButton_A) != 0) {
                    PickFocusedFile();
                } else if ((down & HidNpadButton_Up) != 0) {
                    m_file_list.MoveFocus(-1);
                } else if ((down & HidNpadButton_Down) != 0) {
                    m_file_list.MoveFocus(+1);
                }
                break;
            }
            if (m_install_screen == InstallScreen::Result) {
                break; /* B -- and the footer's Ⓑ -- is the only way off this screen */
            }
            if ((down & HidNpadButton_Y) != 0) {
                ActivateAction(kActionInstallLocal);
            } else if ((down & HidNpadButton_X) != 0) {
                ActivateAction(kActionInstallDownload);
            } else if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionInstallUpload);
            }
            break;
        case Page::Install:
            if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionPrimary);
            } else if ((down & HidNpadButton_B) != 0) {
                ActivateAction(kActionBack);
            }
            break;
        case Page::Uninstall:
            if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionPrimary);
            } else if ((down & HidNpadButton_B) != 0) {
                ActivateAction(kActionBack);
            }
            break;
        case Page::Result:
            if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionPrimary);
            }
            break;
        default:
            break;
    }
}

/* Touch half: press remembers the action under the finger, release inside the same action
   fires it (drag cancels).  TapTracker owns the rule so the host tests can pin it. */
void App::HandleTouch() {
    int x = 0;
    int y = 0;
    int count = 0;
    const bool down = m_touch.Poll(&x, &y, &count);
    const TapResult tap = m_tap.Update(m_actions, down, x, y);
    if (tap.press_edge && tap.pressed_action >= 0) {
        m_focus = tap.pressed_action;
    }
    if (tap.tapped_action >= 0) {
        ActivateAction(tap.tapped_action);
    }
}

/* One place that turns an action id into behaviour, so keys and touch cannot drift apart.
   Several ids are shared by the pages (the primary button, "back"), so the page decides what
   they mean.  Tapping "install now" on the confirmation page used to re-run the *home* page's
   meaning of the primary action, which silently did nothing. */
void App::ActivateAction(int id) {
    if (m_page == Page::StartupCheck) {
        if (id == kActionStartupRepair && !m_startup_repaired) {
            RepairStartup();
        } else if (id == kActionFooterBack) {
            m_should_exit = true;
        }
        return;
    }
    switch (m_page) {
        case Page::StartupCheck:
            return; /* handled above; here only to keep the switch exhaustive */
        case Page::Install:
            if (id == kActionPrimary) {
                RunInstall();
                return;
            }
            if (id == kActionBack) {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
                return;
            }
            break;
        /* The progress page takes no input at all (the engine owns the thread) and the guide
           page is read-only: both fall through to the shared handling below.  The installer's own
           actions (its cards and its list rows) are page-specific and handled there too. */
        case Page::Progress:
        case Page::Guide:
        case Page::GameInstall:
            break;
        case Page::Uninstall:
            if (id == kActionPrimary) {
                RunUninstall();
                return;
            }
            if (id == kActionBack) {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
                return;
            }
            break;
        case Page::Result:
            /* "Done" is the only control here: refresh the state and go back to the status
               page (the key path did this; the touch path forgot to). */
            if (id == kActionPrimary) {
                Collect();
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
                return;
            }
            break;
        case Page::Details:
            if (id == kActionLanguage) {
                ToggleLanguage();
                return;
            }
            break;
        case Page::Home:
            break;
    }
    switch (id) {
        case kActionPrimary:
            switch (m_home_kind) {
                case HomeKind::UpToDate:
                    Collect(); /* nothing to install: refresh what the screen shows */
                    break;
                case HomeKind::GameMissing:
                case HomeKind::Unsupported:
                    /* The one useful thing to do here: offer the installer, because a package the
                       player brings is how both of these states stop being true -- and for a
                       console with no game at all, installing one is the only way forward. */
                    OpenGameInstall();
                    break;
                case HomeKind::NeedsInstall:
                case HomeKind::UpdateAvailable:
                case HomeKind::Repair:
                case HomeKind::Incomplete:
                case HomeKind::Failed:
                    m_page = Page::Install;
                    break;
            }
            break;
        case kActionCheckUpdate: StartUpdateCheck(); break;
        case kActionForwarder:
            if (ForwarderActionBlocked(
                    m_report.applet_type == AppletType_Application,
                    m_forwarder_application_running)) {
                return;
            }
            if (m_forwarder_installed) {
                RemoveForwarderTile();
            } else {
                InstallForwarderTile();
            }
            break;
        case kActionGuide: ToggleGuide(); break;
        case kActionGameInstall: OpenGameInstall(); break;
        /* The three source screens land here next milestone; the entries are drawn and pressable
           already so the page's layout and key map can be tested on hardware. */
        case kActionInstallLocal:
            m_install_screen = InstallScreen::Files;
            m_file_dir = "/";
            RefreshFileList();
            break;
        case kActionInstallPickRow: PickFocusedFile(); break;
        case kActionInstallStart: RunGameInstall(); break;
        case kActionInstallBack: InstallStepBack(); break;
        /* The header's two storage blocks: a tap selects, exactly like L / R does. */
        case kActionInstallTargetSd:
            SetInstallTarget(install::TargetStorage::SdCard);
            break;
        case kActionInstallTargetNand:
            SetInstallTarget(install::TargetStorage::Nand);
            break;
        /* The unfinished install's three entries. */
        case kActionPendingResume: ResumePendingInstall(); break;
        case kActionPendingAbandon: AbandonPendingInstall(); break;
        case kActionPendingPickNew:
            m_install_error.clear();
            m_install_screen = InstallScreen::Choose;
            break;
        case kActionInstallDownload:
            /* The address screen: nothing is downloaded until the player has typed an address,
               because this app does not host or hand out any. */
            EnterInstallUrl();
            break;
        case kActionInstallUrl: StartDownload(); break;
        case kActionInstallRecheck: RefreshNetworkState(); break;
        case kActionInstallUpload: EnterInstallUpload(); break;
        case kActionUninstall:
            if (m_page == Page::Home && m_have_state) {
                m_page = Page::Uninstall;
            }
            break;
        case kActionBack:
            m_page = Page::Home;
            break;
        /* Header tabs: L shows the status page, R the details page.  Switching pages rebuilds
           the action list, so the focus is re-seated on whatever the new page offers. */
        case kActionTabStatus:
            if (m_page != Page::Home) {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
            }
            break;
        case kActionTabDetails:
            if (m_page != Page::Details) {
                m_page = Page::Details;
                m_focus = FirstEnabled(m_actions);
            }
            break;
        /* The footer's Ⓑ hint does exactly what the B key does. */
        case kActionFooterBack:
            if (m_page == Page::Home) {
                m_should_exit = true;
            } else {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
            }
            break;
        default:
            break;
    }
}

void App::Run() {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    m_touch.Init(m_log);
    /* The game installer writes its diagnostics through this sink (one line each); it has nothing
       to do with the agent install path, so it is set here, once, rather than in RunInstall. */
    install::SetGameInstallLog(m_log);
    /* Where the installer's task file lives.  The engine decides when it has to be on the card; the
       card itself is ours (the app owns the SD handle), so the two operations are handed over here
       rather than the path being threaded through the engine. */
    if (m_sd != nullptr) {
        FsFileSystem *sd = m_sd;
        install::SetGameInstallTaskStore(
            [sd](const std::string &json, std::string *error) {
                return install::WriteTextFile(*sd, kTaskPath, json, error);
            },
            [sd](std::string *error) {
                return install::RemoveFileIfPresent(*sd, kTaskPath, error);
            });
    }
    m_focus = FirstEnabled(m_actions);
    int frames_drawn = 0;
    while (appletMainLoop() && !m_should_exit) {
        PollUpdateCheck();
        /* The browser page is answered from this loop: a page that cannot be served while the
           console draws is a page that times out every second. */
        PollInstallerServer();
        padUpdate(&pad);
        const u32 buttons_down = padGetButtonsDown(&pad);
        HandleKeys(buttons_down);
        HandleStick(pad);
        HandleTouch();
        if (m_page != Page::Home) {
            m_home_focus_needs_default = true;
        }
        /* After the input, before the draw: a key or a tap is what changes the screen, and the
           screen that just appeared has to be served before its own first frame is drawn. */
        SyncInstallerServer();
        Render();
        /* The environment block of log.txt goes out after the second frame, not before the first:
           writing it costs about 2.2 s (one open/write/flush/close per line, two sinks), so doing
           it earlier was most of the black screen -- and by waiting one more frame the player also
           gets a frame of input handling before the pause, instead of a picture that ignores them. */
        ++frames_drawn;
        if (!m_report_traced && frames_drawn >= 2 && !m_startup_page) {
            m_report_traced = true;
            TraceEnvironment();
            if (m_log != nullptr) {
                install::dev::LogStartup(*m_log, "environment report written");
            }
        }
    }
}

/* First-frame stage pauses: wait for + before continuing (used to localize a render crash
   to one stage). */
void App::WaitForPlus(const char *stage) {
    Trace("frame: paused after %s (press + to continue)", stage);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
            break;
        }
    }
    Trace("frame: resuming after %s", stage);
}

void App::Render() {
    if (!m_fb_ready) {
        return;
    }
    if (!m_first_frame_traced) {
        m_first_frame_traced = true;
        if (m_log != nullptr) {
            install::dev::LogStartup(*m_log, "first frame drawn (the black screen ends here)");
        }
    }
    u32 stride = 0;
    /* After framebufferMakeLinear you must draw on the shadow buffer returned by
       framebufferBegin; framebufferEnd copies it into the real framebuffer. */
    void *framebuffer = framebufferBegin(&m_fb, &stride);
    const bool trace = m_trace_frames > 0;
    if (trace) {
        Trace("frame: begin (buf=%p stride=%u)", framebuffer, stride);
    }
    Surface surface;
    surface.pixels = static_cast<u32 *>(framebuffer != nullptr ? framebuffer : m_fb.buf);
    surface.width = static_cast<int>(m_fb.width_aligned);
    /* Important: framebufferMakeLinear sizes the linear shadow buffer by the **window
       height** (stride * ((win->height+7)&~7)), while width/height_aligned round up to the
       GOB boundary (720 -> 768 rows).  Drawing at height_aligned writes 48 rows past the
       buffer (about 245 KB per frame) into the process heap shared with the hbl loader,
       which measurably crashed the loader in its stdio buffer path.  So the drawing height
       is the smaller of the window height and height_aligned. */
    surface.height = std::min(static_cast<int>(m_fb.height_aligned),
                              static_cast<int>(m_fb.win->height));
    surface.stride = static_cast<int>(stride / 4);

    Fill(surface, kBackground);
    /* The storage row is read once per frame and shared: the header draws it and the touch targets
       come from it, and reading it twice doubled the cost of the ncm queries for one answer.  The
       flag is cleared at the end of the frame so a stale read can never be drawn. */
    if (m_page == Page::GameInstall) {
        RefreshStorageRow(surface.width);
    }
    if (trace) {
        Trace("frame: fill done");
        if (m_stage_pause) {
            WaitForPlus("fill");
        }
    }
    RenderHeader(surface);
    if (trace) {
        Trace("frame: header done");
        if (m_stage_pause) {
            WaitForPlus("header");
        }
    }
    switch (m_page) {
        case Page::Home: RenderHome(surface); break;
        case Page::StartupCheck: RenderStartupCheck(surface); break;
        case Page::GameInstall: RenderGameInstall(surface); break;
        case Page::Details: RenderDetails(surface); break;
        case Page::Install: RenderInstall(surface); break;
        case Page::Uninstall: RenderUninstall(surface); break;
        case Page::Progress: RenderProgress(surface); break;
        case Page::Guide: RenderGuide(surface); break;
        case Page::Result: RenderResult(surface); break;
    }
    /* Every page shows the same chrome (header tabs, footer hint), added after the page's own
       controls so the entry focus stays on the page's primary action. */
    AddChromeActions(surface.width, surface.height);
    /* The list is rebuilt every frame, so the focus can end up pointing at an action this page
       does not have: switching pages re-seats it from the *old* list, and the details page's
       first action is its language row.  A focus id that is not in the list makes A do nothing
       -- the "A stopped working after pressing R then L" bug.  A disabled control is left alone
       on purpose: the ring stays on it and A does nothing, which is what a greyed-out button
       should do. */
    {
        bool focus_known = false;
        for (const Action &action : m_actions) {
            if (action.id == m_focus) {
                focus_known = true;
                break;
            }
        }
        if (!focus_known) {
            m_focus = FirstEnabled(m_actions);
        }
    }
    if (trace) {
        Trace("frame: body done");
        if (m_stage_pause) {
            WaitForPlus("body");
        }
    }
    RenderFooter(surface);
    if (trace) {
        Trace("frame: footer done");
        if (m_stage_pause) {
            WaitForPlus("footer");
        }
    }
    framebufferEnd(&m_fb);
    m_storage_row_ready = false;
    if (trace) {
        Trace("frame: end done");
        --m_trace_frames;
    }
}

void App::DrawBrand(Surface surface, int limit) {
    const int width = limit - kMargin - 2;
    if (width < 60) {
        /* No room for either line: saying nothing is honest, drawing over the tabs is not. */
        return;
    }
    /* The title's descenders (the 'g' in Manager) reach about 9 px below the baseline, so the
       subtitle starts below them instead of touching them.  Both lines are fitted: an ellipsis in
       the app's own name at one screen width is better than a name painted across the tabs. */
    const std::string title = m_font.Fit(Tr(i18n::StringId::AppTitle), kFontTitle, width, 1);
    m_font.Draw(surface, kMargin, 10, kFontTitle, kOnHeader, title.c_str());
    /* One line, and no version in it: which games this build supports belongs on the screens that
       talk about the game (the home status line, the details page), where it comes from the
       manifest -- a header that names 3.0.3 has to be edited every time the manifest changes. */
    const std::string subtitle =
        m_font.Fit(Tr(i18n::StringId::HeadSubtitle), kFontSmall, width, 1);
    m_font.Draw(surface, kMargin + 2, 66, kFontSmall, kHeaderSubtle, subtitle.c_str());
}

void App::RenderHeader(Surface surface) {
    FillRect(surface, 0, 0, surface.width, kHeaderHeight, kHeader);
    /* No tabs while the engine works: the progress page ignores input (see HandleKeys), and a
       drawn control that cannot be pressed is exactly what this project keeps fixing. */
    if (m_page == Page::Progress) {
        DrawBrand(surface, surface.width);
        return;
    }
    if (m_page == Page::StartupCheck) {
        /* No tabs: the app's pages are not reachable until this precondition is resolved. */
        DrawBrand(surface, surface.width);
        return;
    }
    /* The installer page is the app's one modal page: the four entries are hidden (not greyed
       out) and the room they took shows the two storage bars, so the player sees why the page
       cannot be left with L/R and how much room the install has. */
    if (m_page == Page::GameInstall) {
        DrawBrand(surface, surface.width);
        RenderStorageBars(surface);
        return;
    }
    struct Tab {
        const char *key;
        i18n::StringId label;
        int action;
    };
    /* Order matters three times: it is what the player reads left to right; the device tests find
       the current page through the entry that carries the underline; and the *grouping* says what
       the keys do.  The mode goes first, on its own, then the three pages the ring moves between
       -- so the two entries that do not answer to L / R never sit between the ones that do.  The
       badges say the same thing: + on the installer (a mode, entered deliberately), Ⓛ / Ⓡ on the
       two ends of the ring, and none on the middle of the ring (nothing goes *straight* there, so
       a badge would be a promise no key keeps).  - still opens the guide, the ring's last stop. */
    const Tab tabs[kHeaderTabCount] = {{"+", i18n::StringId::TabInstall, kActionGameInstall},
                                       {"L", i18n::StringId::TabStatus, kActionTabStatus},
                                       {"", i18n::StringId::TabDetails, kActionTabDetails},
                                       {"R", i18n::StringId::TabGuide, kActionGuide}};
    int label_width[kHeaderTabCount] = {};
    for (int i = 0; i < kHeaderTabCount; ++i) {
        label_width[i] = m_font.Measure(Tr(tabs[i].label), kFontHeading);
    }
    const HeaderTabLayout layout = LayoutHeaderTabs(surface.width, label_width, kHeaderTabBadgePos);
    /* The brand block is bounded by the tab strip's left edge: the subtitle is long in some
       languages, and the strip's own left edge is the only thing that knows where "too far" is
       (measured in English: the subtitle ran under the "+ Game Install / Update" badge). */
    DrawBrand(surface, layout.hit[0].x - 24);
    for (int i = 0; i < kHeaderTabCount; ++i) {
        /* Which entry reads as "where I am": the guide while it is open, otherwise status
           everywhere except the details page. */
        bool active = false;
        switch (tabs[i].action) {
            case kActionGuide: active = m_page == Page::Guide; break;
            case kActionGameInstall: active = m_page == Page::GameInstall; break;
            case kActionTabStatus:
                active = m_page != Page::Details && m_page != Page::Guide;
                break;
            case kActionTabDetails: active = m_page == Page::Details; break;
            default: break;
        }
        const char *label = Tr(tabs[i].label);
        m_font.Draw(surface, layout.label_x[i], 36, kFontHeading,
                    active ? kOnHeader : kHeaderSubtle, label);
        if (kHeaderTabBadgePos[i] != TabBadge::None) {
            FillRoundedRect(surface, layout.badge_x[i], kHeaderTabTop, kHeaderTabBadge,
                            kHeaderTabBadge, kHeaderTabBadge / 2,
                            active ? kOnHeader : kHeaderBadge);
            /* Centre the letter on both axes: our Draw() treats y as the top of the line box, so
               bias it by half the box to land the glyph in the middle of the circle. */
            const int key_width = m_font.Measure(tabs[i].key, kFontSmall);
            m_font.DrawBold(surface, layout.badge_x[i] + (kHeaderTabBadge - key_width) / 2,
                            kHeaderTabTop + 10, kFontSmall, active ? kHeader : kOnHeader,
                            tabs[i].key);
        }
        if (active) {
            /* Under the label, wherever the label sits -- a badge may be in front of it, behind
               it, or nowhere, and none of that moves the mark that says "you are here". */
            FillRect(surface, layout.label_x[i], kHeaderHeight - 14, label_width[i], 4, kOnHeader);
        }
    }
}

/* Everything the chrome draws as a button is one: the header tabs and the footer's Ⓑ hint go
   through the same actions as the L / R / B keys (see HandleKeys -> ActivateAction), and the
   drawn pill and the touch target come from one layout function each.  They are appended
   after the page's own controls, which keeps the entry focus on the page's primary action. */
void App::AddChromeActions(int surface_width, int surface_height) {
    /* The progress page has no chrome on purpose: its keys are dead while the engine works, and
       drawing tabs or a "B back" hint would promise controls that do nothing. */
    if (m_page == Page::Progress || m_page == Page::StartupCheck) {
        return;
    }
    /* The installer page hides the tabs and shows the two storage blocks instead; those blocks are
       also the control that picks where the run writes (design section 8.1).  Their rectangles come
       from the same layout the drawing uses -- and they are registered here, not while drawing,
       because the page's own render clears the action list first.  Not on the progress screen:
       there the target is locked in, so a tap must do nothing (the bars are still *drawn* there --
       they say which storage is being written to). */
    if (m_page == Page::GameInstall) {
        if (!InstallTargetSelectable()) {
            return;
        }
        if (!m_storage_row_ready) {
            RefreshStorageRow(surface_width);
        }
        const ui::StorageBarsLayout &layout = m_storage_row;
        /* SD first: the pair reads in the order L / R moves through it.  They have their own
           keys and are not generic spatial-focus targets. */
        m_actions.push_back({kActionInstallTargetSd, layout.panel[0], true, false});
        m_actions.push_back({kActionInstallTargetNand, layout.panel[1], true, false});
        return;
    }
    /* Same order as RenderHeader draws (the installer first, then the ring's three pages): the
       drawn pill and its touch target have to come from one place, and this is the other half. */
    const i18n::StringId header_labels[kHeaderTabCount] = {
        i18n::StringId::TabInstall, i18n::StringId::TabStatus, i18n::StringId::TabDetails,
        i18n::StringId::TabGuide};
    const int header_actions[kHeaderTabCount] = {kActionGameInstall, kActionTabStatus,
                                                 kActionTabDetails, kActionGuide};
    int label_width[kHeaderTabCount] = {};
    for (int i = 0; i < kHeaderTabCount; ++i) {
        label_width[i] = m_font.Measure(Tr(header_labels[i]), kFontHeading);
    }
    /* The same mask the drawing uses: a tab that shows no badge still has to be tappable, and a
       tab that shows one has to be tappable *there*. */
    const HeaderTabLayout layout = LayoutHeaderTabs(surface_width, label_width, kHeaderTabBadgePos);
    for (int i = 0; i < kHeaderTabCount; ++i) {
        m_actions.push_back({header_actions[i], layout.hit[i], true});
    }

    if (!HasOwnBackButton()) {
        const i18n::StringId label_id =
            (m_page == Page::Home || m_page == Page::StartupCheck)
                ? i18n::StringId::LabelExit
                : i18n::StringId::LabelBack;
        const int footer_label = m_font.Measure(Tr(label_id), kFontSmall);
        const FooterHintLayout footer = LayoutFooterHint(
            surface_height, m_font.LineHeight(kFontSmall), footer_label);
        m_actions.push_back({kActionFooterBack, footer.hit, true});
    }
}

/* Two footer hints, side by side, each of them a control: what is drawn as pressable has to be
   pressable.  Both screens that use it are "the card is information, the footer is the control". */
int App::DrawFooterHintPair(Surface surface, int y, i18n::StringId first, int first_action,
                            i18n::StringId second, int second_action) {
    int x = kMargin;
    const auto draw_hint = [&](const char *key, const char *label, Color face, Color ink,
                               int action) {
        FillRoundedRect(surface, x, y + 7, 30, 30, 15, face);
        const int key_width = m_font.Measure(key, kFontSmall);
        m_font.DrawBold(surface, x + (30 - key_width) / 2, y + 13, kFontSmall, ink, key);
        m_font.Draw(surface, x + 42, y + 12, kFontSmall, kText, label);
        const int label_width = m_font.Measure(label, kFontSmall);
        m_actions.push_back({action, Rect{x - 6, y + 3, 48 + label_width + 12, 38}, true});
        x += 42 + label_width + 34;
    };
    draw_hint("A", Tr(first), kHeader, kOnHeader, first_action);
    draw_hint("B", Tr(second), kBorder, kText, second_action);
    return x;
}

void App::RenderFooter(Surface surface) {
    const int y = surface.height - kFooterHeight;
    FillRect(surface, 0, y, surface.width, kFooterHeight, kCard);
    StrokeRect(surface, 0, y, surface.width, 2, 2, kBorder);
    /* The footer keeps one thing the buttons cannot show: how to leave.  B gets the same kind
       of badge the buttons use, so the "one button, one action" rule still holds. */
    /* Two screens of the install flow say something extra here, because the player has to know it
       before pressing anything:
         * the transfer runs on this thread and takes no keys at all, so promising a "back" would
           be a lie -- the footer asks to stay instead, and the page offers no cancel either;
         * the confirm screen can be started with `Ⓐ`, and that hint belongs next to "leave". */
    if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Progress) {
        int wait_seconds = 0;
        const bool waiting = m_upload_source != nullptr &&
                             m_upload_source->PageWaitSeconds(&wait_seconds);
        if (waiting) {
            /* The browser has gone quiet: say how long the console will wait before the run is
               declared failed, so the player is not staring at a frozen screen (2026-10-19). */
            char text[96];
            std::snprintf(text, sizeof(text), Tr(i18n::StringId::ProgressWaitBrowser),
                          wait_seconds);
            const int width = m_font.Measure(text, kFontSmall);
            m_font.Draw(surface, (surface.width - width) / 2, y + 12, kFontSmall, kWarn, text);
        } else {
            const char *hint = Tr(i18n::StringId::ProgressDoNotInterrupt);
            const int hint_width = m_font.Measure(hint, kFontSmall);
            m_font.Draw(surface, (surface.width - hint_width) / 2, y + 12, kFontSmall, kWarn, hint);
        }
        return;
    }
    if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Confirm) {
        /* "Start" is only useful next to "leave", and drawing them at the same spot (which the
           first version did) makes an unreadable pile. */
        const int hints_end = DrawFooterHintPair(surface, y, i18n::StringId::ConfirmStart,
                                                 kActionInstallStart, i18n::StringId::LabelBack,
                                                 kActionInstallBack);
        DrawInstallTargetHint(surface, y + 12, hints_end + 30);
        const int stamp_width = m_font.Measure(m_footer_text, kFontSmall);
        m_font.Draw(surface, surface.width - kMargin - stamp_width, y + 14, kFontSmall, kSubtle,
                    m_footer_text.c_str());
        return;
    }
    if (m_page == Page::GameInstall &&
        m_install_screen == InstallScreen::Upload && m_upload_reading) {
        /* Reading the package blocks this thread, so no key would work; say what the console is
           waiting for instead of promising a Ⓑ that cannot be handled right now. */
        int wait_seconds = 0;
        char text[96];
        const char *line = nullptr;
        bool warn = false;
        if (m_upload_source != nullptr && m_upload_source->PageWaitSeconds(&wait_seconds)) {
            std::snprintf(text, sizeof(text), Tr(i18n::StringId::ProgressWaitBrowser), wait_seconds);
            line = text;
            warn = true;
        } else {
            line = Tr(i18n::StringId::PageUploadInfo);
        }
        const int width = m_font.Measure(line, kFontSmall);
        m_font.Draw(surface, (surface.width - width) / 2, y + 12, kFontSmall,
                    warn ? kWarn : kText, line);
        return;
    }
    if (m_page == Page::GameInstall &&
        (m_install_screen == InstallScreen::Url ||
         (m_install_screen == InstallScreen::Upload && m_install_no_network))) {
        /* The two source screens, while there is a console-side control to offer.  With no network
           neither screen has a working one, so its A is the "connect first" retry; the address
           screen otherwise offers the keyboard.  The upload screen with a network has nothing to
           offer (the browser is the only way in, design section 8) and falls through to the plain
           Ⓑ footer below, which is where every other install screen gets its "leave" hint. */
        const int hints_end =
            DrawFooterHintPair(surface, y,
                               m_install_no_network ? i18n::StringId::InstallRecheck
                                                    : i18n::StringId::InstallTypeUrl,
                               m_install_no_network ? kActionInstallRecheck : kActionInstallUrl,
                               i18n::StringId::LabelBack, kActionInstallBack);
        DrawInstallTargetHint(surface, y + 12, hints_end + 30);
        const int stamp_width = m_font.Measure(m_footer_text, kFontSmall);
        m_font.Draw(surface, surface.width - kMargin - stamp_width, y + 14, kFontSmall, kSubtle,
                    m_footer_text.c_str());
        return;
    }
    /* The confirmation and result pages carry their own Ⓑ button, so the footer stays quiet
       there: one key, one place. */
    const bool has_back_button = HasOwnBackButton();
    const i18n::StringId label_id =
        (m_page == Page::Home || m_page == Page::StartupCheck)
            ? i18n::StringId::LabelExit
            : i18n::StringId::LabelBack;
    const char *label = has_back_button ? "" : Tr(label_id);
    const FooterHintLayout hint = LayoutFooterHint(
        surface.height, m_font.LineHeight(kFontSmall),
        has_back_button ? 0 : m_font.Measure(label, kFontSmall));
    if (!has_back_button) {
        FillRoundedRect(surface, hint.badge_x, hint.badge_y, hint.badge, hint.badge,
                        hint.badge / 2, kBorder);
        const int key_width = m_font.Measure("B", kFontSmall);
        m_font.DrawBold(surface, hint.badge_x + (hint.badge - key_width) / 2, hint.badge_y + 6,
                        kFontSmall, kText, "B");
        /* Measured on the console: the layout's offset puts the label's optical centre on the
           badge's (the glyph box is not the visual box). */
        m_font.Draw(surface, hint.label_x, hint.text_y, kFontSmall, kSubtle, label);
        /* The installer page is the one page whose footer hint is pushed here rather than in
           AddChromeActions (that function skips the whole page, tabs included): its hint is the
           same control as its B key -- the page keeps its own screen stack -- so a tap steps back
           exactly like the key does.  Everywhere else the action is already there. */
        if (m_page == Page::GameInstall) {
            const int width = (hint.label_x - hint.badge_x) + m_font.Measure(label, kFontSmall);
            m_actions.push_back({kActionInstallBack,
                                 Rect{hint.badge_x - 6, hint.badge_y - 4, width + 12,
                                      hint.badge + 8},
                                 true});
        }
    }
    const int stamp_width = m_font.Measure(m_footer_text, kFontSmall);
    const int stamp_x = surface.width - kMargin - stamp_width;
    int language_hint_x = 0;
    int language_hint_width = 0;
    if (m_page == Page::Details) {
        const std::string switch_hint = Tr(i18n::StringId::FooterLanguageSwitch);
        const int switch_width = m_font.Measure(switch_hint, kFontSmall);
        const int switch_x =
            std::max(kMargin,
                     std::min((surface.width - switch_width) / 2,
                              stamp_x - 24 - switch_width));
        m_font.Draw(surface, switch_x, hint.text_y, kFontSmall, kSubtle, switch_hint.c_str());
        language_hint_x = switch_x;
        language_hint_width = switch_width;
    }
    m_font.Draw(surface, stamp_x, hint.text_y, kFontSmall, kSubtle, m_footer_text.c_str());
    /* And how to move between the pages the header lists: the tabs themselves show Ⓛ / Ⓡ on the
       ends of that group, but only the footer has the room to say it in words.  Plain text, not a
       control -- it names two keys, so there is no single action a tap could mean.  Not on the
       installer page (its header hides the tabs and L / R are dead there) and not on the progress
       page (no chrome at all). */
    if (m_page == Page::GameInstall) {
        /* The installer's own footer note, and only where it is true: L / R pick the storage on the
           screens that still let the choice be made, and do nothing at all on the result screen
           (the run is over), so that one gets no note rather than a note about other pages' keys. */
        if (InstallTargetSelectable()) {
            DrawInstallTargetHint(surface, hint.text_y,
                                  hint.label_x + m_font.Measure(label, kFontSmall) + 30);
        }
    } else if (m_page != Page::Progress && m_page != Page::StartupCheck) {
        const char *pages_hint = Tr(i18n::StringId::FooterPagesHint);
        const int hint_width = m_font.Measure(pages_hint, kFontSmall);
        int x = kMargin;
        if (!has_back_button) {
            /* Clear of the Ⓑ hint when that one is there; at the margin when the page carries its
               own back button and the footer is otherwise empty. */
            x = hint.label_x + m_font.Measure(label, kFontSmall) + 30;
        }
        const bool clears_language =
            language_hint_width == 0 || x + hint_width + 24 <= language_hint_x;
        if (clears_language && x + hint_width + stamp_width + 24 < surface.width) {
            m_font.Draw(surface, x, hint.text_y, kFontSmall, kSubtle, pages_hint);
        }
    }
}

/* The install page's footer note: which key changes the target storage.  The tabs are hidden on
   that page, so the "L / R switch pages" note has no meaning there and this one takes its place,
   after the page's own hints (the footer's left group is "how to leave", and this is the same kind
   of hint); `x` comes from the caller because only it knows where its hints ended. */
void App::DrawInstallTargetHint(Surface surface, int text_y, int x) {
    const std::string hint = Tr(i18n::StringId::FooterInstallTargetHint);
    const int hint_width = m_font.Measure(hint, kFontSmall);
    const int stamp_width = m_font.Measure(m_footer_text, kFontSmall);
    if (x + hint_width + stamp_width + 24 >= surface.width) {
        return; /* no room next to the stamps: saying nothing beats drawing over them */
    }
    m_font.Draw(surface, x, text_y, kFontSmall, kSubtle, hint.c_str());
}

void App::RefreshFooterText() {
    m_footer_text = FooterVersionText(kBuildStamp, kAppVersion, i18n::Current());
}

bool App::HasOwnBackButton() const {
    /* The confirmation and result pages draw their own Ⓑ button, so the footer hint is omitted
       there: one key, one place.  The progress page has no back affordance at all -- B cannot
       leave it while the engine works -- so it stays quiet too. */
    return m_page == Page::Install || m_page == Page::Uninstall || m_page == Page::Result ||
           m_page == Page::Progress;
}

void App::Card(Surface surface, int x, int y, int w, int h, const char *title) {
    FillRect(surface, x, y, w, h, kCard);
    StrokeRect(surface, x, y, w, h, 2, kBorder);
    FillRect(surface, x, y, w, 4, kHeader);
    if (title != nullptr) {
        /* Same left inset as the rows underneath, so the card's title and its content share
           one edge (the 6 px difference was visible on hardware). */
        m_font.Draw(surface, x + kCardPadX, y + 14, kFontHeading, kText, title);
        /* (title sits at y+14; the band is 48 so the first row keeps its clearance) */
    }
}

/* The height one row really occupies: the value wraps at value_width and Fit() truncates
   anything past max_lines, so the measured line count and the painted line count always
   agree. */
int App::RowsHeight(const std::vector<Row> &rows, int value_width, int row_gap) {
    int total = 0;
    bool first = true;
    for (const Row &row : rows) {
        /* A null label means the row is all value and spans the whole card body width. */
        const int width = row.label == nullptr ? value_width + kLabelColumn : value_width;
        const std::string text = m_font.Fit(row.value, row.size, width, row.max_lines);
        total += std::max(1, m_font.LineCount(text, row.size, width)) * m_font.LineHeight(row.size);
        if (!first) {
            total += row_gap;
        }
        first = false;
    }
    return total;
}

/* surface is already the card body's clip region (origin at the card's top-left), so this
   only paints the rows themselves. */
void App::DrawRows(Surface surface, const std::vector<Row> &rows, int value_width, int row_gap) {
    int pen_y = 0;
    bool first = true;
    for (const Row &row : rows) {
        if (!first) {
            pen_y += row_gap;
        }
        first = false;
        const bool full_width = row.label == nullptr;
        const int width = full_width ? value_width + kLabelColumn : value_width;
        const std::string text = m_font.Fit(row.value, row.size, width, row.max_lines);
        if (m_trace_frames > 0) {
            Trace("row: label=\"%s\" value=\"%.24s\"", row.label != nullptr ? row.label : "",
                  text.c_str());
        }
        if (full_width) {
            m_font.Draw(surface, kCardPadX, pen_y, row.size, row.color, text, width);
        } else {
            m_font.Draw(surface, kCardPadX, pen_y, row.size, kSubtle, row.label);
            m_font.Draw(surface, kCardPadX + kLabelColumn, pen_y, row.size, row.color, text, width);
        }
        pen_y +=
            std::max(1, m_font.LineCount(text, row.size, width)) * m_font.LineHeight(row.size);
    }
}

int App::DrawCard(Surface page, int y, int bottom, int width, int value_width, const char *title,
                  const std::vector<Row> &rows, int row_gap, bool fill_rest, int bottom_pad) {
    /* Without a title the card does not need the title band: that empty strip used to sit
       above the rows while the file list below was squeezed. */
    const int top_inset = title != nullptr ? kCardHeader : kCardPadTop;
    int height = top_inset + RowsHeight(rows, value_width, row_gap) + bottom_pad;
    if (fill_rest) {
        height = std::max(height, bottom - y);
    }
    height = std::min(height, bottom - y);
    if (height <= 0) {
        return bottom;
    }
    Card(page, kMargin, y, width, height, title);
    /* The body goes into a subview inside the card: coordinates start at 0 and no row can
       cross the border. */
    const Surface body = page.Subview(kMargin, y + top_inset, width, height - top_inset);
    /* Rows but an empty view means the coordinates or the clip are wrong.  That mistake only
       looks like "an empty card", which is hard to tell from "there really is no data", so
       it gets an explicit log line. */
    if (body.pixels == nullptr && !rows.empty()) {
        Trace("card: body is empty but has %zu rows (title=%s y=%d h=%d)", rows.size(), title, y,
              height);
    }
    DrawRows(body, rows, value_width, row_gap);
    return y + height + kCardGap;
}

/* ---------------- home screen: one screen, one action ---------------- */

namespace {

/* Layout of the home screen (see the design sketch: status line, primary button, two
   half-width blocks).  The content margin is the page margin the header, footer and every
   other page use, so the left/right edges line up across the whole app. */
constexpr int kHomeMargin = kMargin;
constexpr int kHomeStatusY = 136;
constexpr int kHomePrimaryY = 270;
constexpr int kHomeButtonHeight = 128;
constexpr int kHomeSecondaryHeight = 100;
constexpr int kHomeSecondaryGap = 20;
constexpr int kHomeForwarderHeight = kHomeSecondaryHeight;
constexpr int kHomeForwarderGap = 14;
constexpr int kHomeGap = 24;

/* The legacy-cheat strip.  Home carries one message that is not a state: a cheat file left in
   the game directory drives the same in-game input path as the agent, so missing that message
   changes what happens in the game.  It therefore sits above the buttons rather than being
   folded into the state line (a healthy install and a leftover cheat file are both true at
   once), and the buttons step down for it instead of the strip being squeezed into the gap.
   The geometry is derived from the status block so a font or copy change cannot make them
   overlap: the status block ends at `status_y + title line + 6 + body line`. */
constexpr int kHomeStatusBlockBottom =
    kHomeStatusY + (kFontTitle + kFontTitle / 3) + 6 + (kFontBody + kFontBody / 3);
/* The line is the path, so it is long: one size below the body text keeps the whole strip on one
   line in both languages (the English label is the longer of the two). */
constexpr int kHomeWarnFont = kFontSmall;
constexpr int kHomeWarnPad = 18; /* above and below the line, inside the border */
constexpr int kHomeWarnHeight =
    (kHomeWarnFont + kHomeWarnFont / 3) + kHomeWarnPad * 2;
constexpr int kHomeWarnY = kHomeStatusBlockBottom + 12;
/* What the two button rows move down while the strip is shown: exactly the room it takes plus the
   gap that keeps it off the primary button. */
constexpr int kHomeWarnShift = (kHomeWarnY + kHomeWarnHeight + 10) - kHomePrimaryY;
static_assert(kHomeWarnShift > 0, "the warning strip has to push the buttons down");
constexpr int kHomeButtonsBottom =
    kHomePrimaryY + kHomeWarnShift + kHomeButtonHeight + kHomeSecondaryGap +
    kHomeSecondaryHeight + kHomeForwarderGap + kHomeForwarderHeight;
static_assert(kHomeButtonsBottom <= 720 - kFooterHeight - 8,
              "the home buttons have to clear the footer even with the warning strip");

/* Key badge: the physical button that also triggers this control, drawn inside it. */
void DrawKeyBadge(Surface surface, Font &font, int cx, int cy, int radius, const char *key,
                  Color face, Color text) {
    FillRoundedRect(surface, cx - radius, cy - radius, radius * 2, radius * 2, radius, face);
    const int width = font.Measure(key, kFontBody);
    font.DrawBold(surface, cx - width / 2, cy - kFontBody / 2 - 1, kFontBody, text, key);
}

/* Eight dots in a ring, one step every eighth of a second.  The clock phase keeps the speed
   independent of the frame rate; the in-flight update check uses this in the key-badge slot. */
void DrawSpinner(Surface surface, int cx, int cy, int radius, Color color) {
    constexpr int kDots = 8;
    constexpr double kTwoPi = 6.283185307179586;
    const u64 ticks = armGetSystemTick();
    const u64 phase = (ticks / (armGetSystemTickFreq() / 8)) % kDots;
    for (int i = 0; i < kDots; ++i) {
        const int step = (static_cast<int>(phase) + i) % kDots;
        const double angle = (kTwoPi * step) / kDots - kTwoPi / 4;
        const int x = cx + static_cast<int>(radius * std::cos(angle));
        const int y = cy + static_cast<int>(radius * std::sin(angle));
        const u8 alpha = static_cast<u8>(40 + (215 * (kDots - i)) / kDots);
        FillRoundedRect(surface, x - 3, y - 3, 7, 7, 3,
                        Color{color.r, color.g, color.b, alpha});
    }
}

/* The attention mark of the home warning strip: a filled triangle with an exclamation mark
   knocked out of it.  The drawing layer has no polygon primitive, so the triangle is painted one
   scanline at a time.  `size` is its height and the base is a fifth wider -- an equilateral
   warning triangle is wider than tall, and the first version of this mark (base twice the height)
   read as a stretched blob on the console.  `cut` is the strip's own fill colour, so the mark
   works on any background the strip is given. */
void DrawWarnMark(Surface surface, int cx, int cy, int size, Color color, Color cut) {
    const int top = cy - size / 2;
    const int base = (size * 6) / 5;
    for (int row = 0; row < size; ++row) {
        const int width = ((row + 1) * base) / size;
        FillRect(surface, cx - width / 2, top + row, width, 1, color);
    }
    /* The glyph is centred in the triangle's interior: a stem, a gap, and a dot.  Its rows are
       measured down from the apex so a change of `size` keeps the proportions. */
    constexpr int kStroke = 3;
    constexpr int kDot = 3;
    constexpr int kGap = 3;
    const int stem_height = size / 3;
    const int glyph_height = stem_height + kGap + kDot;
    const int stem_top = top + (size - glyph_height) / 2;
    FillRect(surface, cx - kStroke / 2, stem_top, kStroke, stem_height, cut);
    FillRect(surface, cx - kStroke / 2, stem_top + stem_height + kGap, kStroke, kDot, cut);
}

/* Centre a single line in a rectangle.  The line box is what gets centred, so the ink lands in
   the optical middle -- the old fixed "-26 px" offset left every button label about 10 px high
   (measured on the confirmation page). */
/* Measured on the console: the glyph box carries descender space, so centring the line box
   still leaves the ink about 6 px high inside a 110 px button. */
constexpr int kLabelOpticalBias = 6;

void DrawCenteredLabel(Surface surface, Font &font, const Rect &rect, int size, Color color,
                       const std::string &text) {
    const int width = font.Measure(text.c_str(), size);
    const int line = font.LineHeight(size);
    font.Draw(surface, rect.x + (rect.w - width) / 2,
              rect.y + (rect.h - line) / 2 + kLabelOpticalBias, size, color, text);
}

}  // namespace

void App::BuildHomeActions(int width, int height) {
    (void)height;
    const int inner = width - kHomeMargin * 2;
    const int half = (inner - kHomeGap) / 2;
    const bool installed = AgentInstalled();
    /* A greyed-out primary button means "nothing to do": already up to date, or this build
       cannot be handled at all. */
    /* Unsupported keeps an enabled primary button: it opens the game installer instead of doing
       nothing, which is the whole point of 1.1.0 for a console stuck on 1.0.0. */
    const bool primary_enabled = m_home_kind != HomeKind::UpToDate;
    /* One shift for both rows, and it lives here because the touch targets are built from the
       same rectangles: moving the buttons in the renderer alone would leave the finger hitting
       the old place. */
    const int shift = m_report.legacy_cheat_present ? kHomeWarnShift : 0;
    const int primary_y = kHomePrimaryY + shift;
    const int secondary_y = primary_y + kHomeButtonHeight + kHomeSecondaryGap;
    const int forwarder_y = secondary_y + kHomeSecondaryHeight + kHomeForwarderGap;
    const bool forwarder_action_blocked = ForwarderActionBlocked(
        m_report.applet_type == AppletType_Application, m_forwarder_application_running);
    m_actions.clear();
    m_actions.push_back(
        {kActionPrimary, Rect{kHomeMargin, primary_y, inner, kHomeButtonHeight},
         primary_enabled});
    /* Without the uninstall button there is nothing to pair with, and a half-width button next
       to empty space reads as a missing control, so the check button takes the full row. */
    const int check_width = installed ? half : inner;
    m_actions.push_back({kActionCheckUpdate,
                         Rect{kHomeMargin, secondary_y, check_width,
                              kHomeSecondaryHeight},
                         true});
    if (installed) {
        m_actions.push_back({kActionUninstall,
                             Rect{kHomeMargin + half + kHomeGap, secondary_y, half,
                                  kHomeSecondaryHeight},
                             true});
    }
    m_actions.push_back({kActionForwarder,
                         Rect{kHomeMargin, forwarder_y, inner, kHomeForwarderHeight},
                         !forwarder_action_blocked});
}

void App::MoveHomeFocus(int direction) {
    if (m_page != Page::Home) {
        return;
    }
    std::vector<Action> focus_actions;
    for (const Action &action : m_actions) {
        if (IsHomeFocusAction(action.id)) {
            focus_actions.push_back(action);
        }
    }
    if (m_focus < 0) {
        m_focus = acnh_manager::ui::DefaultHomeFocus(focus_actions, kActionPrimary);
        return;
    }
    const int next =
        acnh_manager::ui::MoveHomeFocus(focus_actions, m_focus, direction, kActionPrimary);
    if (next >= 0) {
        m_focus = next;
    }
}

void App::NormalizeHomeFocus() {
    if (m_page != Page::Home) {
        return;
    }
    std::vector<Action> focus_actions;
    for (const Action &action : m_actions) {
        if (IsHomeFocusAction(action.id)) {
            focus_actions.push_back(action);
        }
    }
    for (const Action &action : focus_actions) {
        if (action.id == m_focus && action.enabled) {
            return;
        }
    }
    m_focus = acnh_manager::ui::DefaultHomeFocus(focus_actions, kActionPrimary);
}

void App::RenderHome(Surface surface) {
    BuildHomeActions(surface.width, surface.height);
    if (m_home_focus_needs_default) {
        m_focus = -1;
        m_home_focus_needs_default = false;
    }
    NormalizeHomeFocus();
    /* Colour carries the state: teal = ready to act, green = fine, amber = needs attention,
       red = something failed. */
    Color dot = kHeader;
    switch (m_home_kind) {
        case HomeKind::GameMissing:
        case HomeKind::Unsupported:
        case HomeKind::Incomplete:
        case HomeKind::Repair: dot = kWarn; break;
        case HomeKind::Failed: dot = kBad; break;
        case HomeKind::UpdateAvailable:
        case HomeKind::UpToDate: dot = kGood; break;
        case HomeKind::NeedsInstall: dot = kUninstalled; break;
    }

    /* Headline + the one line of context a player cares about (the two versions). */
    i18n::StringId headline = i18n::StringId::StateNotInstalled;
    switch (m_home_kind) {
        case HomeKind::GameMissing: headline = i18n::StringId::StateGameMissing; break;
        case HomeKind::Unsupported: headline = i18n::StringId::StateUnsupported; break;
        case HomeKind::Failed: headline = i18n::StringId::StateFailed; break;
        case HomeKind::Incomplete: headline = i18n::StringId::StateIncomplete; break;
        case HomeKind::Repair: headline = i18n::StringId::StateRepair; break;
        case HomeKind::NeedsInstall: headline = i18n::StringId::StateNotInstalled; break;
        case HomeKind::UpdateAvailable: headline = i18n::StringId::StateUpdateAvailable; break;
        case HomeKind::UpToDate: headline = i18n::StringId::StateInstalled; break;
    }
    const std::string game_version = InstalledGameVersion();
    /* Two different questions, two different answers: "what is on the card" (the install
       record) and "what a tap would install" (the manifest in use).  They only diverge after a
       network update, and calling the version this build carries "installed" would be wrong. */
    const std::string installed_agent =
        m_have_state ? m_state.agent_version : std::string(Tr(i18n::StringId::ValueNotInstalled));
    const std::string offer_agent =
        (m_have_manifest || m_have_update_manifest)
            ? ActiveManifest().agent.version
            : std::string(Tr(i18n::StringId::ValueNotInstalled));
    std::string second_line;
    switch (m_home_kind) {
        case HomeKind::Repair:
            /* State the facts only, and make the two sides distinguishable: the version alone
               can repeat when only the build changed, so each side carries the first bytes of
               its payload hash -- the same value the installer verifies against. */
            /* Format() is variadic: %s needs a const char*, and the temporary strings live
               until the end of this full expression. */
            second_line = i18n::Format(i18n::StringId::SubRepair,
                                       BuildLabel(m_state.agent_version, InstalledPayloadHash()).c_str(),
                                       BuildLabel(m_manifest.agent.version, ManifestPayloadHash()).c_str());
            break;
        case HomeKind::GameMissing:
            second_line = Tr(i18n::StringId::StateGameMissingSub);
            break;
        case HomeKind::Unsupported: {
            /* "Not supported" on its own leaves the player comparing nothing: name the version
               that is installed and the one this build accepts.  When there is no *different*
               version to name (no manifest, or a refusal that is not about the version), the line
               falls back to pointing at the details page. */
            const std::string supported = SupportedGameVersionNote();
            second_line = supported.empty()
                              ? std::string(Tr(i18n::StringId::StateUnsupportedSub))
                              : i18n::Format(i18n::StringId::HomeVersionsUnsupported,
                                             game_version.c_str(), supported.c_str());
            break;
        }
        case HomeKind::Failed:
            second_line = Tr(i18n::StringId::SubRetry);
            break;
        case HomeKind::NeedsInstall:
            /* Nothing is installed yet, so the version on the right is what the tap would
               install -- saying just "agent 0.11.0" reads like it is already there. */
            second_line = i18n::Format(i18n::StringId::HomeVersionsAvailable,
                                       game_version.c_str(), offer_agent.c_str());
            break;
        case HomeKind::UpdateAvailable:
            second_line = i18n::Format(i18n::StringId::HomeVersionsUpdate, game_version.c_str(),
                                       installed_agent.c_str(), NewerAgentVersion().c_str());
            break;
        default:
            second_line = i18n::Format(i18n::StringId::HomeVersions, game_version.c_str(),
                                       installed_agent.c_str());
            break;
    }

    /* Status block: the dot is centred on the headline, and the versions sit right under it
       (the two lines belong together, so there is no room for a gap).  The version line lines
       up with the headline, not with the dot. */
    const int status_top = kHomeStatusY;
    const int headline_height = m_font.LineHeight(kFontTitle);
    /* The state dot is an indicator, not a control: at 44 px it read like a button.  32 px with
       the text pulled in to match keeps the same optical gap. */
    /* 24 px: clearly an indicator, not a control.  The colour already differs per state --
       teal = ready to install, green = fine, amber = needs attention, red = failed -- see the
       switch above. */
    constexpr int kStatusDot = 24;
    constexpr int kStatusTextX = 44;
    FillRoundedRect(surface, kHomeMargin, status_top + headline_height / 2 - kStatusDot / 2,
                    kStatusDot, kStatusDot, kStatusDot / 2, dot);
    m_font.Draw(surface, kHomeMargin + kStatusTextX, status_top, kFontTitle, kText, Tr(headline));
    m_font.Draw(surface, kHomeMargin + kStatusTextX, status_top + headline_height + 6, kFontBody, kSubtle,
                second_line);

    /* The one message on this screen that is not the install state: an old cheat code left in the
       game directory drives the same in-game input path as the agent, and a missing warning here
       changes what happens in the game.  The warning is about that code, not about the file it
       happens to sit in -- a file full of unrelated entries is not reported at all (see
       env/cheat_file.*) -- but it still names the file by its full path on the card, because that
       is what the player has to open: a bare file name says nothing about where it lives. */
    if (m_report.legacy_cheat_present) {
        const int left = kHomeMargin;
        const int width = surface.width - kHomeMargin * 2;
        constexpr int kBorderWidth = 3;
        constexpr int kRadius = 16;
        FillRoundedRect(surface, left, kHomeWarnY, width, kHomeWarnHeight, kRadius, kWarnBright);
        FillRoundedRect(surface, left + kBorderWidth, kHomeWarnY + kBorderWidth,
                        width - kBorderWidth * 2, kHomeWarnHeight - kBorderWidth * 2,
                        kRadius - kBorderWidth, kWarnTint);
        const int line = m_font.LineHeight(kHomeWarnFont);
        /* No kLabelOpticalBias here: that bias corrects a button label inside a 110 px control, and
           on this strip it pushed the copy visibly below the mark.  The line box is what has to
           line up with the mark; the before/after measurements are in docs/device-acceptance.md. */
        const int text_y = kHomeWarnY + (kHomeWarnHeight - line) / 2;
        DrawWarnMark(surface, left + 38, kHomeWarnY + kHomeWarnHeight / 2, 24, kWarnBright,
                     kWarnTint);
        const char *label = Tr(i18n::StringId::HomeLegacyCheat);
        const int label_width =
            m_font.DrawBold(surface, left + 64, text_y, kHomeWarnFont, kWarn, label);
        m_font.Draw(surface, left + 64 + label_width + 16, text_y, kHomeWarnFont, kWarn,
                    m_report.legacy_cheat_path);
    }

    /* The focused button is filled and carries the only key badge on the page. */
    const Action *primary = nullptr;
    const Action *check = nullptr;
    const Action *uninstall = nullptr;
    const Action *forwarder = nullptr;
    for (const Action &action : m_actions) {
        if (action.id == kActionPrimary) primary = &action;
        if (action.id == kActionCheckUpdate) check = &action;
        if (action.id == kActionUninstall) uninstall = &action;
        if (action.id == kActionForwarder) forwarder = &action;
    }

    auto block = [&](const Action *action, const char *label, const std::string &sub,
                     bool busy = false) {
        if (action == nullptr) {
            return;
        }
        const bool focused = action->id == m_focus;
        const Color face =
            !action->enabled ? kCardDisabled : (focused ? kHeader : kCard);
        const Color border = action->enabled ? (focused ? kHeader : kBorder) : kBorder;
        const Color text = !action->enabled ? kSubtle : (focused ? kOnHeader : kText);
        constexpr int thickness = 3;
        FillRoundedRect(surface, action->rect.x, action->rect.y, action->rect.w, action->rect.h,
                        18, border);
        FillRoundedRect(surface, action->rect.x + thickness, action->rect.y + thickness,
                        action->rect.w - thickness * 2, action->rect.h - thickness * 2, 18 - thickness,
                        face);
        if (busy) {
            DrawSpinner(surface, action->rect.x + 46,
                        action->rect.y + action->rect.h / 2, 13,
                        focused ? kOnHeader : kHeader);
        } else if (focused) {
            DrawKeyBadge(surface, m_font, action->rect.x + 46,
                         action->rect.y + action->rect.h / 2, 22, "A",
                         focused ? kOnHeader : kHeader,
                         focused ? kHeader : kOnHeader);
        }
        constexpr int kButtonLabelSize = kFontHeading + 8;
        const int label_width = m_font.Measure(label, kButtonLabelSize);
        const int centre = action->rect.x + action->rect.w / 2;
        const int label_line = m_font.LineHeight(kButtonLabelSize);
        const int sub_line = m_font.LineHeight(kFontSmall);
        const int text_top =
            action->rect.y + (action->rect.h - label_line - sub_line) / 2 + kLabelOpticalBias;
        m_font.Draw(surface, centre - label_width / 2, text_top, kButtonLabelSize, text, label);
        const int sub_width = m_font.Measure(sub, kFontSmall);
        m_font.Draw(surface, centre - sub_width / 2, text_top + label_line,
                    kFontSmall, focused && action->enabled ? kOnHeader : kSubtle, sub);
    };

    /* One switch per line instead of nesting: the button has to match the state, and the state
       list keeps growing (the "files are gone" case was added last). */
    i18n::StringId label_id = i18n::StringId::BtnUnavailable;
    i18n::StringId sub_id = i18n::StringId::SubInstall;
    switch (m_home_kind) {
        case HomeKind::UpdateAvailable:
            label_id = i18n::StringId::BtnUpdate; /* formatted below: it carries the version */
            break;
        case HomeKind::NeedsInstall: label_id = i18n::StringId::BtnInstall; break;
        case HomeKind::Repair:
        case HomeKind::Incomplete:
            label_id = i18n::StringId::BtnRepair;
            /* The reason lives on the status line, not under the button. */
            sub_id = i18n::StringId::SubInstall;
            break;
        case HomeKind::Failed:
            label_id = i18n::StringId::BtnRetry;
            sub_id = i18n::StringId::SubRetry;
            break;
        case HomeKind::GameMissing:
            /* No game at all: the one useful thing this screen can offer is installing one, which
               is what the app is for.  "Check again" asked the player to find a copy elsewhere
               first, which is exactly what a missing-game console cannot do. */
            label_id = i18n::StringId::BtnInstallGame;
            sub_id = i18n::StringId::SubInstallGame;
            break;
        case HomeKind::UpToDate:
            label_id = i18n::StringId::BtnUpToDate;
            sub_id = i18n::StringId::SubUpToDate;
            break;
        case HomeKind::Unsupported:
            /* The old subtitle ("the reason is in Details") is gone everywhere: this state now
               has something to do, and the button says what it is. */
            label_id = i18n::StringId::HomeInstallUpdate;
            sub_id = i18n::StringId::HomeInstallUpdateSub;
            break;
    }
    const std::string primary_label =
        m_home_kind == HomeKind::UpdateAvailable
            ? i18n::Format(i18n::StringId::BtnUpdate, NewerAgentVersion().c_str())
            : std::string(Tr(label_id));
    const std::string primary_sub = Tr(sub_id);
    block(primary, primary_label.c_str(), primary_sub);
    block(check, Tr(i18n::StringId::BtnCheckUpdate), UpdateSubtitle(), m_update_running);
    if (uninstall != nullptr) {
        block(uninstall, Tr(i18n::StringId::BtnUninstall), Tr(i18n::StringId::SubUninstall));
    }
    const bool forwarder_action_blocked = ForwarderActionBlocked(
        m_report.applet_type == AppletType_Application, m_forwarder_application_running);
    const bool forwarder_process_running =
        m_forwarder_installed && m_forwarder_application_running;
    block(forwarder,
          Tr(m_forwarder_installed ? i18n::StringId::BtnForwarderRemove
                                   : i18n::StringId::BtnForwarderInstall),
          Tr(forwarder_process_running
                 ? i18n::StringId::SubForwarderProcessRunning
                 : (forwarder_action_blocked
                        ? i18n::StringId::SubForwarderRemoveBlocked
                        : (m_forwarder_installed ? i18n::StringId::SubForwarderRemove
                                                 : i18n::StringId::SubForwarderInstall))));
    if (m_trace_frames > 0) {
        Trace("home: rendered kind=%d actions=%zu focus=%d", static_cast<int>(m_home_kind),
              m_actions.size(), m_focus);
    }
}

void App::RenderDetails(Surface surface) {
    /* The details page is read-only: drop the previous page's controls so a tap on empty space
       cannot fire a button that is not drawn here (its rows, header tabs aside, are not
       buttons). */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    /* The game on this console.  When the manifest knows this title and lists a different version,
       the row carries both: the home screen states "not supported" and points here, so whoever
       answers the question finds what the installed version was compared against.  The second
       version rides along in the existing row (same shape as the agent row above) instead of
       taking a row of its own, which would push the English page down a font size. */
    std::string game = Tr(i18n::StringId::ValueNotAvailable);
    if (!m_report.build.title_id.empty()) {
        const std::string name = "Animal Crossing: New Horizons " + InstalledGameVersion();
        const std::string supported = SupportedGameVersionNote();
        game = (m_gate.status != install::GateStatus::Supported && !supported.empty())
                   ? i18n::Format(i18n::StringId::ValueGameVersionSupported, name.c_str(),
                                  supported.c_str())
                   : name;
    }
    /* The version on the card first -- that is the fact about this console; the commit is a
       development detail and lives in the log.  When this build of the app carries a different
       release (a network update went further than the app), the same row says so, which is the
       one difference a person reading this page needs to understand. */
    std::string agent =
        m_have_state ? m_state.agent_version : std::string(Tr(i18n::StringId::ValueNotInstalled));
    /* Only the release compiled into this build counts as "bundled"; a newer manifest fetched
       from the release host is something to install, not something the app carries. */
    const std::string carried = m_manifest_embedded ? m_manifest.agent.version : std::string();
    if (m_have_state && !carried.empty() && carried != agent) {
        agent = i18n::Format(i18n::StringId::ValueAgentCarried, agent.c_str(), carried.c_str());
    }
    char record[96];
    if (m_have_state) {
        std::snprintf(record, sizeof(record), Tr(i18n::StringId::ValueInstallRecord),
                      static_cast<int>(m_state.files.size()));
    } else {
        std::snprintf(record, sizeof(record), "%s", Tr(i18n::StringId::ValueNoInstallRecord));
    }

    std::vector<Row> version_rows;
    version_rows.push_back({Tr(i18n::StringId::LabelGameVersion), game, kText, 2});
    version_rows.push_back({Tr(i18n::StringId::LabelHos), m_report.hos_version, kText, 1});
    /* Where this NRO was launched from.  The startup page fixes a non-standard layout before the
       app is usable; this row is what shows the result afterwards. */
    {
        const bool known = !m_startup.current_path.empty();
        const bool standard = env::IsStandardNroPath(m_startup.current_path);
        version_rows.push_back({Tr(i18n::StringId::LabelProgramLocation),
                                known ? Tr(standard ? i18n::StringId::ValueStandard
                                                    : i18n::StringId::ValueNonstandard)
                                      : Tr(i18n::StringId::ValueNotAvailable),
                                known ? (standard ? kGood : kWarn) : kWarn, 1});
    }
    version_rows.push_back(
        {Tr(i18n::StringId::LabelHomeMenu),
         Tr(m_forwarder_installed ? i18n::StringId::ValueInstalled
                                  : i18n::StringId::ValueNotInstalled),
         m_forwarder_installed ? kGood : kSubtle, 1});
    /* Diagnostics: the applet mode and whether the game is open both matter when something
       behaves differently than expected, so they stay available here. */
    /* Plain words instead of the raw enum name: people know "album (applet) mode" (started
       from the album) versus "application (full memory) mode". */
    version_rows.push_back(
        {Tr(i18n::StringId::LabelAppletMode),
         Tr(m_report.applet_type == AppletType_Application ? i18n::StringId::AppletModeApplication
                                                           : i18n::StringId::AppletModeLibrary),
         kText, 1});
    version_rows.push_back({Tr(i18n::StringId::LabelGameRunning),
                            Tr(m_report.application_running ? i18n::StringId::ValuePresent
                                                            : i18n::StringId::ValueAbsent),
                            m_report.application_running ? kWarn : kText, 1});
    version_rows.push_back(
        {Tr(i18n::StringId::LabelContentId),
         m_report.build.content_id.empty() ? Tr(i18n::StringId::ValueNotAvailable)
                                           : m_report.build.content_id,
         m_report.build.content_id.empty() ? kWarn : kGood, 2});
    version_rows.push_back({Tr(i18n::StringId::LabelEmbeddedAgent), agent, kText, 1});
    version_rows.push_back({Tr(i18n::StringId::LabelInstallRecord), record, kText, 1});
    /* The unfinished install, when there is one: the placeholders it holds are invisible on the SD
       card *and* in "free space", so this row is the only place the number is explained.  The two
       actions that resolve it live on the install page (`＋`), which the row points at. */
    if (m_have_pending_task) {
        const std::string pending =
            i18n::Format(i18n::StringId::PendingProgress,
                         util::FormatSize(m_pending_live_bytes).c_str(),
                         util::FormatSize(install::TotalBytes(m_pending_task)).c_str()) +
            " · " +
            i18n::Format(i18n::StringId::PendingReserved,
                         util::FormatSize(m_pending_live_reserved).c_str());
        version_rows.push_back({Tr(i18n::StringId::PendingTitle), pending, kWarn, 2});
    }
    version_rows.push_back({Tr(i18n::StringId::LabelOverrideConfig), m_report.advice.text,
                            m_report.advice.never_applies ? kWarn : kGood, 2});
    version_rows.push_back({Tr(i18n::StringId::LabelLogPath), "/switch/ACNH-Manager/log.txt",
                            kSubtle, 1});
    /* The home screen promises "the reason is in Details", so the gate verdict and any
       detection problem have to be here. */
    version_rows.push_back(
        {Tr(i18n::StringId::LabelGateResult),
         GateResultText(),
         m_gate.status == install::GateStatus::Supported ? kGood : kWarn, 2});
    std::string problems = m_report.problems;
    if (!m_manifest_error.empty()) {
        problems += (problems.empty() ? "" : "; ") + m_manifest_error;
    }
    if (!m_state_error.empty()) {
        problems += (problems.empty() ? "" : "; ") + m_state_error;
    }
    if (!problems.empty()) {
        version_rows.push_back({Tr(i18n::StringId::LabelProblems), problems, kWarn, 2});
    }
    if (m_report.legacy_cheat_present) {
        version_rows.push_back(
            {Tr(i18n::StringId::LabelLegacyCheat), m_report.legacy_cheat_path, kWarn, 2});
    }

    std::vector<Row> advanced_rows;
    advanced_rows.push_back(
        {Tr(i18n::StringId::LanguageLabel),
         Tr(m_language == i18n::Language::ZhHans ? i18n::StringId::LanguageChinese
                                                 : i18n::StringId::LanguageEnglish),
         kText, 1});
    /* Which manifest the next install would actually use -- not which one happens to sit in
       .rodata: while a verified newer release is adopted, the files come from the network, and
       a row that keeps saying "bundled with the app" is the kind of lie that costs an hour of
       support time. */
    std::string manifest_source;
    if (m_have_update_manifest) {
        manifest_source = i18n::Format(i18n::StringId::ValueNetworkManifest,
                                       ActiveManifest().agent.version.c_str());
    } else if (m_manifest_embedded) {
        manifest_source = std::string(Tr(i18n::StringId::ValueEmbeddedManifest));
    } else {
        manifest_source = m_dev_manifest_path + (m_allow_dev_manifest ? "  [dev allowed]" : "");
    }
    advanced_rows.push_back({Tr(i18n::StringId::LabelManifestSource), manifest_source, kText, 1});
    advanced_rows.push_back({Tr(i18n::StringId::LabelUpdateCheck), UpdateDetailText(),
                             kText, 2});

    /* The details page carries every row we keep; the language switch can make the text longer
       (the English override advice wraps to two lines) and the lower card used to be clipped.
       The tightening order lives in one table instead of a chain of ifs: row gap first, then
       the bottom padding, then the whole page one size down (16 px is the measured minimum that
       still reads right, and smaller type is still readable where a clipped row is not).
       Clipping remains the last resort (docs/architecture.md section 7). */
    struct LayoutTier {
        int row_gap;
        int bottom_pad;
        int font;
    };
    const LayoutTier tiers[] = {
        {kRowGap, kCardPadBottom, kFontBody},
        {0, kCardPadBottom, kFontBody},
        {0, 16, kFontBody},
        {0, 16, kFontSmall}, /* one size down beats cutting the last row off the card */
    };
    /* Row carries its own size, so the smaller step needs its own copies of both lists. */
    std::vector<Row> version_rows_small = version_rows;
    std::vector<Row> advanced_rows_small = advanced_rows;
    for (Row &row : version_rows_small) {
        row.size = kFontSmall;
    }
    for (Row &row : advanced_rows_small) {
        row.size = kFontSmall;
    }
    const int available = page_bottom - page_top - kCardGap;
    LayoutTier tier = tiers[sizeof(tiers) / sizeof(tiers[0]) - 1];
    const std::vector<Row> *version_shown = &version_rows_small;
    const std::vector<Row> *advanced_shown = &advanced_rows_small;
    for (const LayoutTier &candidate : tiers) {
        const bool small = candidate.font == kFontSmall;
        const std::vector<Row> &version_at = small ? version_rows_small : version_rows;
        const std::vector<Row> &advanced_at = small ? advanced_rows_small : advanced_rows;
        const int needed = kCardHeader + RowsHeight(version_at, value_width, candidate.row_gap) +
                           candidate.bottom_pad + kCardHeader +
                           RowsHeight(advanced_at, value_width, candidate.row_gap) +
                           candidate.bottom_pad;
        if (needed <= available) {
            tier = candidate;
            /* Points at the vectors declared above the loop -- not at anything local to this
               iteration. */
            version_shown = small ? &version_rows_small : &version_rows;
            advanced_shown = small ? &advanced_rows_small : &advanced_rows;
            break;
        }
    }
    const int row_gap = tier.row_gap;
    const int bottom_pad = tier.bottom_pad;
    int y = DrawCard(page, page_top, page_bottom, width, value_width,
                     Tr(i18n::StringId::SectionInstall), *version_shown, row_gap, false, bottom_pad);
    const int advanced_top = y;
    DrawCard(page, y, page_bottom, width, value_width, Tr(i18n::StringId::SectionAdvanced),
             *advanced_shown, row_gap, false, bottom_pad);
    /* The language row is a control, so it is tappable like everything else: one row, one
       action, same code path as the A key on this page. */
    const int row_height = m_font.LineHeight(tier.font);
    m_actions.push_back({kActionLanguage,
                         Rect{kMargin, advanced_top + kCardHeader, width, row_height + kRowGap},
                         true, /*focusable=*/true});
    /* The row is a control, so it carries its key badge like every other button. */
    DrawKeyBadge(page, m_font, kMargin + width - 34,
                 advanced_top + kCardHeader + row_height / 2 + 4, 15, "A", kBorder, kText);
}
/* Confirmation pages use the same language as the home screen: what will happen, then one
   real button (plus a way back for touch users).  Paths, hashes and the raw plan reason stay
   in Details -- the person pressing the button only needs the outcome. */
void App::BuildConfirmActions(int width, int height, bool uninstall) {
    (void)uninstall;
    const int inner = width - kHomeMargin * 2;
    const int half = (inner - kHomeGap) / 2;
    const int top = height - kFooterHeight - kCardGap - kHomeSecondaryHeight;
    m_actions.clear();
    m_actions.push_back(
        {kActionPrimary, Rect{kHomeMargin, top, half, kHomeSecondaryHeight}, true});
    m_actions.push_back({kActionBack, Rect{kHomeMargin + half + kHomeGap, top, half,
                                           kHomeSecondaryHeight},
                         true});
}

void App::RenderConfirm(Surface surface, bool uninstall) {
    BuildConfirmActions(surface.width, surface.height, uninstall);
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::string action;
    if (uninstall) {
        action = std::string(Tr(i18n::StringId::BtnUninstall));
    } else if (m_plan.action == install::PlanAction::Repair) {
        action = std::string(Tr(i18n::StringId::BtnRepair));
    } else if (!NewerAgentVersion().empty()) {
        action = i18n::Format(i18n::StringId::BtnUpdate, NewerAgentVersion().c_str());
    } else {
        action = std::string(Tr(i18n::StringId::BtnInstall));
    }

    std::vector<Row> rows;
    rows.push_back({nullptr, action, kText, 1, kFontTitle});
    /* What will happen first, then what it means afterwards -- the note is the consequence,
       so it reads wrong above the line it follows from. */
    if (!uninstall) {
        rows.push_back({nullptr,
                        i18n::Format(i18n::StringId::ConfirmWillInstall,
                                     ActiveManifest().agent.version.c_str()),
                        kSubtle, 1});
    } else {
        rows.push_back({nullptr, Tr(i18n::StringId::SubUninstall), kSubtle, 1});
    }
    rows.push_back({nullptr, Tr(uninstall ? i18n::StringId::ConfirmUninstallNote
                                          : i18n::StringId::ConfirmInstallNote),
                    kSubtle, 2});
    const int body_bottom = page_bottom - kHomeSecondaryHeight - kCardGap;
    /* No card title here: the big action line already says "install the agent" / "remove the
       agent", so a title line above it was pure repetition. */
    int y = DrawCard(page, page_top, body_bottom, width, value_width, nullptr, rows, kRowGap,
                     false, kCardPadBottom);

    /* The file list stays: people asked for it, and it is the honest answer to "what is this
       going to write?".  Paths and hashes are fine here -- this is the page they are choosing
       to look at, unlike the home screen. */
    const int list_height = std::max(0, body_bottom - y);
    if (list_height > kCardHeader) {
        /* The uninstall page must not say "files to write": nothing is being written. */
        Card(page, kMargin, y, width, list_height,
             Tr(uninstall ? i18n::StringId::LabelPlanRemove : i18n::StringId::LabelPlan));
        const Surface list = page.Subview(kMargin, y + kCardHeader, width, list_height - kCardHeader);
        /* Entries are laid out from the data, not from what happens to fit: the fixed 52 px row
           used to cut the third file off without a word.  Many files tighten the rows first and
           end with an explicit "and N more" line as the last resort. */
        /* One line per file: "name -> path   size".  The hash lives in state.json and on the
           details page; here it was noise nobody reads. */
        std::vector<std::string> entries;
        if (!uninstall && m_plan.game != nullptr) {
            for (const auto &file : m_plan.game->files) {
                entries.push_back(file.name + "  →  " + file.target + "    " +
                                  FormatSize(file.size));
            }
        } else if (m_have_state) {
            for (const auto &file : m_state.files) {
                /* Same "name -> path" shape as the install page; the record only stores the
                   target, so the name is its last segment. */
                const std::size_t slash = file.target.find_last_of('/');
                const std::string name =
                    slash == std::string::npos ? file.target : file.target.substr(slash + 1);
                entries.push_back(name + "  →  " + file.target + "    " +
                                  FormatSize(file.size));
            }
        }
        if (entries.empty()) {
            m_font.Draw(list, kCardPadX, 0, kFontBody, kWarn, Tr(i18n::StringId::ValueNone),
                        list.width - kCardPadX * 2);
        } else {
            const int count = static_cast<int>(entries.size());
            /* Single-line rows: tight between each other, with a clear gap under the card's
               title (the first row starts below kListTopGap). */
            int entry_height = 32;
            while (entry_height > 24 && count * entry_height + kCardPadBottom > list.height) {
                entry_height -= 4;
            }
            int visible = count;
            while (visible > 1 &&
                   visible * entry_height + kCardPadBottom + kListTopGap > list.height) {
                --visible;
            }
            const bool clipped = visible < count;
            const int rows_space =
                clipped ? list.height - entry_height : list.height; /* room for the note */
            while (clipped && visible * entry_height + kCardPadBottom + kListTopGap > rows_space) {
                --visible;
            }
            int row = kListTopGap;
            for (int i = 0; i < visible; ++i) {
                m_font.Draw(list, kCardPadX, row, kFontSmall, kText, entries[i],
                            list.width - kCardPadX * 2);
                row += entry_height;
            }
            if (clipped) {
                m_font.Draw(list, kCardPadX, row + 4, kFontSmall, kSubtle,
                            i18n::Format(i18n::StringId::LabelPlanMore, count - visible).c_str(),
                            list.width - kCardPadX * 2);
            }
        }
    }

    /* The two buttons, drawn exactly like the home screen's. */
    for (const Action &action_item : m_actions) {
        const bool primary = action_item.id == kActionPrimary;
        const Color face = primary ? kHeader : kCard;
        const Color border = primary ? kHeader : kBorder;
        const Color text = primary ? kOnHeader : kText;
        const int thickness = m_focus == action_item.id ? 6 : 3;
        FillRoundedRect(surface, action_item.rect.x, action_item.rect.y, action_item.rect.w,
                        action_item.rect.h, 18, border);
        FillRoundedRect(surface, action_item.rect.x + thickness, action_item.rect.y + thickness,
                        action_item.rect.w - thickness * 2, action_item.rect.h - thickness * 2,
                        18 - thickness, face);
        const char *key = primary ? "A" : "B";
        DrawKeyBadge(surface, m_font, action_item.rect.x + 46,
                     action_item.rect.y + action_item.rect.h / 2, 22, key,
                     primary ? kOnHeader : dot_face(), primary ? kHeader : kOnHeader);
        const char *label = Tr(primary ? (uninstall ? i18n::StringId::ConfirmRemove
                                                    : i18n::StringId::ConfirmStart)
                                       : i18n::StringId::LabelBack);
        DrawCenteredLabel(surface, m_font, action_item.rect, kFontHeading + 8, text, label);
    }
}

void App::RenderInstall(Surface surface) { RenderConfirm(surface, false); }

void App::RenderUninstall(Surface surface) { RenderConfirm(surface, true); }

/* The progress page exists because the install/uninstall engine runs on the frame-loop thread:
   nothing can animate while it works, so the honest thing to do is say what is happening.  One
   frame goes up before the first byte moves and one after every file (see BeginProgress /
   UpdateProgress); a player watching a network download therefore sees "Installing the agent /
   subsdk9 (1/3)" instead of a frozen confirmation page. */
void App::BeginProgress(bool uninstall, int total) {
    m_progress_uninstall = uninstall;
    m_progress_step.clear();
    m_progress_index = 0;
    m_progress_total = total;
    m_page = Page::Progress;
    Render();
}

/* Called from the engine's callback: same thread as the frame loop, between two files, so
   nothing else is drawing or reading the card at that moment. */
void App::UpdateProgress(const install::Progress &progress) {
    m_progress_step = progress.step;
    m_progress_index = progress.index;
    m_progress_total = progress.total;
    Render();
}

/* Paints the recovered code as squares: one filled rect per black module, on a white plate.
   `module` is the on-screen size of one module, so the caller picks how large it lands.  The
   matrix comes from either of the two sources in this build -- the mini-app's code recovered from
   its image (`ui::QrMatrix`) and the installer's address encoded at runtime (`util::QrCode`) --
   which is why this is a template over the two `Size()` / `At(row, column)` readers. */
template <typename Matrix>
void DrawQrCode(Surface surface, const Matrix &qr, int x, int y, int module) {
    const int side = qr.Size() * module;
    FillRect(surface, x, y, side + module * 2, side + module * 2, Color{0xFF, 0xFF, 0xFF, 0xFF});
    for (int row = 0; row < qr.Size(); ++row) {
        for (int column = 0; column < qr.Size(); ++column) {
            if (qr.At(row, column)) {
                FillRect(surface, x + module + column * module, y + module + row * module, module,
                         module, Color{0x00, 0x00, 0x00, 0xFF});
            }
        }
    }
}

struct QrPlateLayout {
    int module{0};
    int code_x{0};
    int code_y{0};
    int code_side{0};
};

template <typename Matrix>
QrPlateLayout DrawQrCodePlate(Surface surface, const Matrix &qr, int x, int y, int plate_side) {
    FillRect(surface, x, y, plate_side, plate_side, Color{0xFF, 0xFF, 0xFF, 0xFF});
    QrPlateLayout out;
    if (qr.Empty()) {
        return out;
    }
    constexpr int kQuietModules = 2;
    out.module = std::max(1, std::min(7, plate_side / (qr.Size() + kQuietModules * 2)));
    out.code_side = qr.Size() * out.module;
    out.code_x = x + (plate_side - out.code_side) / 2;
    out.code_y = y + (plate_side - out.code_side) / 2;
    for (int row = 0; row < qr.Size(); ++row) {
        for (int column = 0; column < qr.Size(); ++column) {
            if (qr.At(row, column)) {
                FillRect(surface, out.code_x + column * out.module,
                         out.code_y + row * out.module, out.module, out.module,
                         Color{0x00, 0x00, 0x00, 0xFF});
            }
        }
    }
    return out;
}

/* The chat-code explainer.  Text first (what a code is, how to send one, where the numbers come
   from), then the mini-app's code with its caption and the guide address for people who would
   rather not install WeChat. */
void App::RenderGuide(Surface surface) {
    /* Read-only page: no controls of its own (the header tabs and the footer come from the
       shared chrome, and both go through the action table). */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> rows;
    rows.push_back({nullptr, Tr(i18n::StringId::GuideTitle), kText, 1, kFontTitle});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideWhat), kText, 2});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideHow), kText, 2});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideHowMore), kText, 2});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideWhere), kText, 2});
    /* Let DrawCard size the card: it adds the top and bottom padding itself and only clamps to
       `bottom`.  Handing it a pre-computed bottom that already included one padding made it
       clamp away the *top* padding, which left the last line sitting on the card's edge. */
    const int text_card_bottom =
        DrawCard(page, page_top, page_bottom, width, value_width, nullptr, rows, kRowGap, false,
                 kCardPadBottom);

    /* The two scan routes share one card: mini-app code and copy on the left, web-guide copy and
       code mirrored on the right.  A fixed plate size keeps both codes visually equal. */
    const int card_top = text_card_bottom; /* DrawCard's return value already skips the gap */
    constexpr int kQrCardPad = 12;
    constexpr int kQrPlateMax = 216;
    const int available = page_bottom - card_top;
    const int plate_side = std::clamp(available - kQrCardPad * 2, 160, kQrPlateMax);
    const int card_height = std::max(plate_side + kQrCardPad * 2, 120);
    FillRoundedRect(page, kMargin, card_top, width, card_height, 14, kCard);
    StrokeRect(page, kMargin, card_top, width, card_height, 3, kBorder);

    const bool has_mini = !m_guide_qr.Empty();
    const bool has_web = !m_guide_web_qr.Empty();
    if (has_mini || has_web) {
        const int plate_y = card_top + (card_height - plate_side) / 2;
        const int centre_y = card_top + card_height / 2;
        const int divider_x = kMargin + width / 2;
        if (has_mini && has_web) {
            FillRect(page, divider_x, card_top + 20, 2, card_height - 40, kBorder);
        }

        constexpr int kCopyLineGap = 4;
        constexpr int kCopyBodyMaxLines = 3;
        const int mini_text_x = kMargin + kCardPadX + plate_side + 20;
        const int mini_text_right = has_web ? divider_x - 24 : kMargin + width - kCardPadX;
        const int web_text_x = has_mini ? divider_x + 24 : kMargin + kCardPadX;
        const int web_text_right =
            kMargin + width - kCardPadX - (has_web ? plate_side + 20 : 0);
        const auto copy_height = [&](bool present, i18n::StringId id, int max_width) {
            if (!present) {
                return 0;
            }
            const int lines =
                std::clamp(m_font.LineCount(Tr(id), kFontSmall, max_width), 1,
                           kCopyBodyMaxLines);
            return kFontHeading + 8 + lines * m_font.LineHeight(kFontSmall);
        };
        const int tallest_copy =
            std::max(copy_height(has_mini, i18n::StringId::GuideScan,
                                 std::max(1, mini_text_right - mini_text_x)),
                     copy_height(has_web, i18n::StringId::GuideWebScan,
                                 std::max(1, web_text_right - web_text_x)));
        const int copy_top = centre_y - tallest_copy / 2;
        const auto copy = [&](int x, int max_width, int top, i18n::StringId title,
                              std::initializer_list<i18n::StringId> lines, bool align_right) {
            int y = top;
            const std::string title_text = m_font.Fit(Tr(title), kFontHeading, max_width, 1);
            m_font.DrawBold(page,
                            align_right
                                ? x + max_width -
                                      m_font.Measure(title_text, kFontHeading, max_width)
                                : x,
                            y, kFontHeading, kText, title_text.c_str(), max_width);
            y += kFontHeading + 8;
            for (const i18n::StringId id : lines) {
                const std::string line =
                    m_font.Fit(Tr(id), kFontSmall, max_width, kCopyBodyMaxLines);
                m_font.Draw(page,
                            align_right
                                ? x + max_width - m_font.Measure(line, kFontSmall, max_width)
                                : x,
                            y, kFontSmall, kSubtle, line.c_str(), max_width);
                y += m_font.LineHeight(kFontSmall) + kCopyLineGap;
            }
        };

        const auto mark = [&](const QrPlateLayout &layout, i18n::StringId id) {
            constexpr int kMarkPlateW = 80;
            constexpr int kMarkPlateH = 52;
            constexpr int kMarkFont = 24;
            constexpr int kMarkBiasY = 5;
            const int qr_centre_x = layout.code_x + layout.code_side / 2;
            const int qr_centre_y = layout.code_y + layout.code_side / 2;
            FillRoundedRect(page, qr_centre_x - kMarkPlateW / 2,
                            qr_centre_y - kMarkPlateH / 2, kMarkPlateW, kMarkPlateH, 8,
                            Color{0xFF, 0xFF, 0xFF, 0xFF});
            const std::string text = m_font.Fit(Tr(id), kMarkFont, kMarkPlateW, 1);
            const int text_width = m_font.Measure(text, kMarkFont);
            m_font.DrawBold(page, qr_centre_x - text_width / 2,
                            qr_centre_y - m_font.LineHeight(kMarkFont) / 2 + kMarkBiasY,
                            kMarkFont, kText, text.c_str());
        };

        if (has_mini) {
            const int qr_x = kMargin + kCardPadX;
            const QrPlateLayout layout =
                DrawQrCodePlate(page, m_guide_qr, qr_x, plate_y, plate_side);
            if (layout.module > 0) {
                mark(layout, i18n::StringId::GuideMark);
            }
            copy(mini_text_x, mini_text_right - mini_text_x, copy_top,
                 i18n::StringId::GuideMiniAppTitle,
                 {i18n::StringId::GuideScan}, true);
        }

        if (has_web) {
            const int qr_x = kMargin + width - kCardPadX - plate_side;
            const QrPlateLayout layout =
                DrawQrCodePlate(page, m_guide_web_qr, qr_x, plate_y, plate_side);
            if (layout.module > 0) {
                mark(layout, i18n::StringId::GuideWebMark);
            }
            copy(web_text_x, web_text_right - web_text_x, copy_top,
                 i18n::StringId::GuideWebTitle,
                 {i18n::StringId::GuideWebScan}, false);
        }
    } else {
        /* No code in this build: say where the numbers come from instead of leaving a hole. */
        m_font.Draw(page, kMargin + kCardPadX, card_top + 26, kFontBody, kText,
                    m_font.Fit(Tr(i18n::StringId::GuideWhere), kFontBody, value_width, 3),
                    value_width);
    }
}

/* One place for "open the chat-code explainer, or leave it": the + key and the header's "+"
   entry both come through here, so they cannot drift apart. */
void App::ToggleGuide() {
    if (m_page == Page::Guide) {
        m_page = m_guide_return;
    } else {
        m_guide_return = m_page;
        m_page = Page::Guide;
        Trace("guide: opened from page %d", static_cast<int>(m_guide_return));
    }
    m_focus = FirstEnabled(m_actions);
}

/* L / R: one step through the pages the player reads, in the order the header draws them --
   status, details, guide -- wrapping at both ends.  A page outside the ring (the installer, the
   agent install flow) enters it at the first page, so the keys always move somewhere instead of
   being dead.  Entering the guide through the ring records where we came from, exactly like the
   - key does, so B on the guide returns to the page the player was reading. */
void App::SwitchPeerPage(int direction) {
    const Page peers[acnh_manager::ui::kPeerTabCount] = {Page::Home, Page::Details, Page::Guide};
    int current = -1;
    for (int i = 0; i < acnh_manager::ui::kPeerTabCount; ++i) {
        if (m_page == peers[i]) {
            current = i;
            break;
        }
    }
    const Page target = peers[acnh_manager::ui::CyclePeerTab(current, direction)];
    if (target == Page::Guide) {
        m_guide_return = current >= 0 ? peers[current] : m_page;
        Trace("guide: opened from page %d", static_cast<int>(m_guide_return));
    }
    m_page = target;
    m_focus = FirstEnabled(m_actions);
}


/* The game installer: the app's one modal page.  Entering it remembers the page we came from
   (B returns there) and, while it is up, the header hides the four entries and shows the storage
   bars instead -- the page is exclusive, so offering a way out that is not B would be a lie. */

/* True when `title_id` belongs to a title this build supports.  The manifest lists the *base*
   id while a package's CNMT carries the patch id (base + 0x800) or a DLC id (base + 0x1000), so
   comparing the strings would say "not ours" for our own game's update.  The family is the high
   48 bits -- the same rule the running-game check uses. */

/* True when this build's agent supports `version` of `title_id`: same title family, and the
   version the manifest lists -- the gate compares versions exactly, so a 2.0.0 update of our own
   game is still not something the agent can attach to. */
bool App::AgentSupportedFor(const std::string &title_id, std::uint32_t version) const {
    const unsigned long long wanted = std::strtoull(title_id.c_str(), nullptr, 16);
    if (wanted == 0) {
        return false;
    }
    for (const auto &game : ActiveManifest().games) {
        const unsigned long long known = std::strtoull(game.title_id.c_str(), nullptr, 16);
        if (known != 0 && (known >> 16) == (wanted >> 16) && game.version == version) {
            return true;
        }
    }
    return false;
}

std::string App::AgentSupportedVersionsFor(const std::string &title_id) const {
    const unsigned long long wanted = std::strtoull(title_id.c_str(), nullptr, 16);
    if (wanted == 0) {
        return "";
    }
    std::string joined;
    for (const auto &game : ActiveManifest().games) {
        const unsigned long long known = std::strtoull(game.title_id.c_str(), nullptr, 16);
        if (known == 0 || (known >> 16) != (wanted >> 16)) {
            continue;
        }
        if (game.display_version.empty()) {
            continue;
        }
        if (joined.find(game.display_version) != std::string::npos) {
            continue; /* the manifest may list the same game twice (profiles) */
        }
        if (!joined.empty()) {
            joined += " / ";
        }
        joined += game.display_version;
    }
    return joined;
}

bool App::TitleIsOurs(const std::string &title_id) const {
    const unsigned long long wanted = std::strtoull(title_id.c_str(), nullptr, 16);
    if (wanted == 0) {
        return false;
    }
    for (const auto &game : ActiveManifest().games) {
        const unsigned long long known = std::strtoull(game.title_id.c_str(), nullptr, 16);
        if (known != 0 && (known >> 16) == (wanted >> 16)) {
            return true;
        }
    }
    return false;
}

void App::OpenGameInstall() {
    if (m_page == Page::GameInstall) {
        return;
    }
    m_game_install_return = m_page;
    m_page = Page::GameInstall;
    /* The installer is a modal, long-lived page (file browsing, address typing, waiting for a
       phone); keep the screen awake from the moment it opens, not only once a transfer starts. */
    DisableAutoSleep();
    /* The page's server does *not* come up here: picking a file from the card never needs it, and
       the address and code are only printed on the two source screens (see SyncInstallerServer). */
    /* An interrupted install comes first: it holds space on one of the storages, and the only two
       ways to get that space back (or to finish what was started) are on that screen. */
    RefreshPendingTask();
    m_install_screen = m_have_pending_task ? InstallScreen::Pending : InstallScreen::Choose;
    m_install_origin = InstallScreen::Choose;
    m_install_returns_to_launcher = false;
    m_file_dir = "/";
    m_focus = FirstEnabled(m_actions);
    Trace("install: opened from page %d", static_cast<int>(m_game_install_return));
}

void App::RefreshForwarderTile() {
    m_forwarder_installed = false;
    char title_id[17];
    std::snprintf(title_id, sizeof(title_id), "%016llx",
                  static_cast<unsigned long long>(env::StandardForwarderTitleId()));
    install::ContentInstaller probe(install::TargetStorage::SdCard, install::InstallOptions{});
    m_forwarder_application_running = false;
    std::string process_error;
    bool process_running = false;
    if (probe.ApplicationRunning(title_id, install::MetaType::Application, &process_running,
                                 &process_error)) {
        m_forwarder_application_running = process_running;
    } else {
        Trace("forwarder: running-state query unavailable: %s", process_error.c_str());
    }
    std::string error;
    if (!probe.Open(&error)) {
        Trace("forwarder: installed-state query unavailable: %s", error.c_str());
        return;
    }
    bool exact = false;
    std::uint32_t highest = 0;
    if (!probe.InstalledState(title_id, 0, 0x80, &exact, &highest, &error)) {
        Trace("forwarder: installed-state query failed: %s", error.c_str());
        return;
    }
    m_forwarder_installed = exact || highest != 0;
    Trace("forwarder: title=%s installed=%d version=0x%x", title_id,
          m_forwarder_installed ? 1 : 0, static_cast<unsigned>(highest));
}

void App::InstallForwarderTile() {
    m_result_forwarder = false;
    m_game_install_return = m_page;
    m_install_returns_to_launcher = true;
    const std::string_view nsp = payload::EmbeddedForwarderNsp();
    if (nsp.empty()) {
        m_install_error = Tr(i18n::StringId::InstallForwarderUnavailable);
        m_page = Page::GameInstall;
        m_install_screen = InstallScreen::Result;
        Trace("forwarder: the embedded package is missing");
        return;
    }

    m_page = Page::GameInstall;
    m_install_screen = InstallScreen::Choose;
    m_install_origin = InstallScreen::Choose;
    m_install_page_drives = false;
    DisableAutoSleep();

    std::string cleanup_error;
    if (!DeleteApplicationTitles({env::LegacyForwarderTitleId()}, &cleanup_error)) {
        Trace("forwarder: legacy cleanup failed: %s", cleanup_error.c_str());
    }

    m_memory_source = std::make_unique<install::MemorySource>();
    std::string error;
    if (!m_memory_source->Open(reinterpret_cast<const std::uint8_t *>(nsp.data()), nsp.size(),
                               &error)) {
        m_install_error = error;
        m_install_screen = InstallScreen::Result;
        Trace("forwarder: embedded package open failed: %s", error.c_str());
        return;
    }

    const install::TargetStorage saved_target = m_install_target;
    m_install_target = install::TargetStorage::SdCard;
    OpenPackage(*m_memory_source, "ACNH-Manager-forwarder.nsp", nsp.size(),
                install::TaskSource::Internal, std::string());
    m_install_target = saved_target;

    if (m_game_install != nullptr && m_install_screen == InstallScreen::Confirm) {
        Trace("forwarder: starting install title=%016llx",
              static_cast<unsigned long long>(env::StandardForwarderTitleId()));
        RunGameInstall();
    } else if (m_game_install == nullptr && m_install_screen == InstallScreen::Choose) {
        /* Prepare refused before it had a package.  A running game gets the existing actionable
           result card; anything else returns home instead of leaving the installer's source
           screen up, which would look like ZR opened the game installer. */
        if (m_install_error == install::kErrorGameRunning) {
            m_install_screen = InstallScreen::Result;
        } else {
            m_page = m_game_install_return;
            m_focus = FirstEnabled(m_actions);
        }
    }
}

void App::RemoveForwarderTile() {
    const std::uint64_t title_id = env::StandardForwarderTitleId();
    char title_hex[17];
    std::snprintf(title_hex, sizeof(title_hex), "%016llx",
                  static_cast<unsigned long long>(title_id));
    install::ContentInstaller probe(install::TargetStorage::SdCard, install::InstallOptions{});
    const auto shortcut_running = [&]() {
        bool running = false;
        std::string query_error;
        return probe.ApplicationRunning(title_hex, install::MetaType::Application, &running,
                                        &query_error) &&
               running;
    };
    const auto show_failure = [&](const std::string &reason) {
        m_result_forwarder = true;
        m_result_uninstall = false;
        m_result_ok = false;
        m_result_files = 0;
        m_result_error = reason;
        Trace("forwarder: remove failed title=%016llx error=%s",
              static_cast<unsigned long long>(title_id), reason.c_str());
        Collect();
        m_page = Page::Result;
        m_focus = FirstEnabled(m_actions);
    };

    if (shortcut_running()) {
        show_failure(Tr(i18n::StringId::ResultForwarderRemoveRunning));
        return;
    }

    std::string error;
    const bool ok = DeleteApplicationTitles(
        {title_id, env::LegacyForwarderTitleId()}, &error);
    m_result_forwarder = true;
    m_result_uninstall = false;
    m_result_ok = ok;
    m_result_files = 0;
    if (ok) {
        m_result_error.clear();
        Trace("forwarder: removed title=%016llx",
              static_cast<unsigned long long>(title_id));
    } else {
        m_result_error = shortcut_running() ? Tr(i18n::StringId::ResultForwarderRemoveRunning)
                                            : Tr(i18n::StringId::ResultForwarderRemoveSystem);
        Trace("forwarder: remove failed title=%016llx error=%s",
              static_cast<unsigned long long>(title_id), error.c_str());
    }
    Collect();
    m_page = Page::Result;
    m_focus = FirstEnabled(m_actions);
}

/* The SD card as a flat list of what the player can pick: ".." to go up, then directories, then
   the files this app knows how to install.  Anything else is shown dimmed rather than hidden --
   a file that is simply absent reads as "the app cannot see my card". */
void App::RefreshFileList() {
    m_file_list.SetItems({});
    if (m_sd == nullptr) {
        return;
    }
    std::vector<ui::ListItem> items;
    if (m_file_dir != "/") {
        items.push_back({"..", Tr(i18n::StringId::InstallUpOneLevel), true});
    }
    const util::FsPath dir(m_file_dir);
    FsDir directory{};
    if (R_FAILED(fsFsOpenDirectory(m_sd, dir.c_str(),
                                   FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &directory))) {
        Trace("install: cannot open %s", m_file_dir.c_str());
        m_file_list.SetItems(std::move(items));
        return;
    }
    FsDirectoryEntry entries[64]{};
    s64 total = 0;
    if (R_SUCCEEDED(fsDirRead(&directory, &total, 64, entries))) {
        for (s64 i = 0; i < total && i < 64; ++i) {
            const std::string name = entries[i].name;
            if (entries[i].type == FsDirEntryType_Dir) {
                items.push_back({name + "/", Tr(i18n::StringId::InstallDirectory), true});
            } else if (entries[i].type == FsDirEntryType_File) {
                const bool supported = install::IsSupportedExtension(name);
                /* The note says what this file *is* and how big it is: a picker whose only number
                   is the file name cannot answer "is this the update I mean?" before the confirm
                   page, and two packages of the same game differ mostly in that number. */
                std::string note = supported ? std::string(Tr(i18n::StringId::InstallPackage))
                                             : std::string(Tr(i18n::StringId::InstallIgnored));
                note += " · ";
                note += util::FormatSize(static_cast<std::uint64_t>(entries[i].file_size));
                items.push_back({name, std::move(note), supported});
            }
        }
    }
    fsDirClose(&directory);
    /* The card's own order is whatever the file system feels like; sort so "..", directories and
       then files read the same way twice, which is what makes a picker predictable. */
    std::stable_sort(items.begin() + (m_file_dir == "/" ? 0 : 1), items.end(),
                     [](const ui::ListItem &lhs, const ui::ListItem &rhs) {
                         const bool lhs_dir = !lhs.name.empty() && lhs.name.back() == '/';
                         const bool rhs_dir = !rhs.name.empty() && rhs.name.back() == '/';
                         if (lhs_dir != rhs_dir) {
                             return lhs_dir;
                         }
                         return lhs.name < rhs.name;
                     });
    m_file_list.SetItems(std::move(items));
    Trace("install: %s -> %zu entries", m_file_dir.c_str(), m_file_list.Items().size());
}

/* "microSD  12.3 / 29.1 GB" and how full that storage is, for each of the two content storages
   (the same call the space check uses, so the screen and the check cannot disagree), read fresh:
   the header shows this on every install screen, and a stale number is worse than none.  A storage
   that cannot be read reports `ok = false` instead of a made-up zero. */
void App::StorageBarTexts(std::string text[2], bool ok[2], double used[2]) const {
    const install::TargetStorage targets[2] = {install::TargetStorage::SdCard,
                                              install::TargetStorage::Nand};
    const i18n::StringId labels[2] = {i18n::StringId::StorageSdCard,
                                     i18n::StringId::StorageSystem};
    for (int i = 0; i < 2; ++i) {
        std::uint64_t free_bytes = 0;
        std::uint64_t total_bytes = 0;
        std::string error;
        ok[i] = install::ContentInstaller::Space(targets[i], &free_bytes, &total_bytes, &error);
        used[i] = ok[i] && total_bytes > 0
                      ? 1.0 - static_cast<double>(free_bytes) / static_cast<double>(total_bytes)
                      : 0.0;
        /* "free / total" in GB with one decimal, the shape the design fixes; a storage that cannot
           be read says so instead of showing 0.0 (design section 8). */
        char caption[64];
        if (ok[i]) {
            std::snprintf(caption, sizeof(caption), "%.1f / %.1f GB",
                          static_cast<double>(free_bytes) / (1024.0 * 1024.0 * 1024.0),
                          static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0));
        } else {
            std::snprintf(caption, sizeof(caption), "%s", Tr(i18n::StringId::StorageUnavailable));
        }
        text[i] = std::string(Tr(labels[i])) + "  " + caption;
    }
}

void App::StorageRowReadout(std::string text[2], int text_w[2], bool ok[2], double used[2],
                            int *label_w) {
    StorageBarTexts(text, ok, used);
    for (int i = 0; i < 2; ++i) {
        text_w[i] = m_font.Measure(text[i], kFontSmall);
    }
    /* The label is drawn bold, which is the same glyph one pixel right and down: two pixels of
       slack keep the layout from tucking a block under it. */
    *label_w = m_font.Measure(Tr(i18n::StringId::InstallToLabel), InstallToLabelSize()) + 2;
}

void App::RefreshStorageRow(int surface_width) {
    int label_w = 0;
    StorageRowReadout(m_storage_text, m_storage_text_w, m_storage_ok, m_storage_used,
                      &label_w);
    m_storage_row = ui::LayoutStorageBars(surface_width, label_w, m_storage_text_w);
    m_storage_row_ready = true;
}

/* The label is the heading for the pair, so it is one size up from the numbers it introduces. */
int App::InstallToLabelSize() const {
    return kFontHeading + 4;
}

/* Centred on the panels beside it, not on the caption's line: the label names the whole group, and
   the group's optical centre is the panel's. */
int App::InstallToLabelY() const {
    return ui::kStoragePanelCenterY - m_font.LineHeight(InstallToLabelSize()) / 2 + 4;
}

/* The row where the tabs were: the fixed "install to:" label and the two storage blocks (microSD,
   the console's own memory).  Each block is a control group -- key badge, "free / total" and the
   used bar -- and the chosen one carries a panel behind it.

   They are also the control that picks where the run writes (design section 8.1), which is why the
   measurement lives in helpers instead of in this drawing loop: the drawing and the touch targets
   have to agree about where each group is, or an idle tap picks the storage the player was not
   pointing at. */
void App::RenderStorageBars(Surface surface) {
    if (!m_storage_row_ready) {
        RefreshStorageRow(surface.width);
    }
    const std::string *text = m_storage_text;
    const bool *ok = m_storage_ok;
    const double *used = m_storage_used;
    const ui::StorageBarsLayout &layout = m_storage_row;
    /* The fixed label that says what the pair is.  It is not a control: nothing registers it, and
       it stays put whichever storage is selected. */
    m_font.DrawBold(surface, layout.label_x, InstallToLabelY(), InstallToLabelSize(), kOnHeader,
                    Tr(i18n::StringId::InstallToLabel));
    const install::TargetStorage targets[2] = {install::TargetStorage::SdCard,
                                              install::TargetStorage::Nand};
    for (int i = 0; i < 2; ++i) {
        /* The panel behind the chosen block is drawn on every screen that shows the pair, including
           the ones where the choice is settled (the transfer, and the result): it says *where this
           is going*, which is worth knowing while several gigabytes are moving, not just what may
           still be tapped.  What does change with the screen is the key badge (letter vs dot, below)
           and the footer's key note (see RenderFooter), and whether a tap does anything (see
           AddChromeActions).  The other block keeps its numbers but dims, so the pair still compares
           -- which is what the numbers are for -- while one of them is clearly the chosen one. */
        const bool selected = m_install_target == targets[i];
        if (selected) {
            FillRoundedRect(surface, layout.panel[i].x, layout.panel[i].y, layout.panel[i].w,
                            layout.panel[i].h, ui::kStoragePanelRadius, kHeaderPanel);
        }
        /* The key that picks this storage, drawn the way every other key in this app is drawn: the
           same disc, in the same place relative to the thing it triggers.  Where the choice is
           settled (the transfer, the result) the keys do nothing, so the disc loses its letter and
           turns into a plain dot -- the same "this control is not available here" statement the
           footer's missing key note makes, instead of a letter that promises a key that does
           nothing.  The two dots are not the same colour: the chosen storage keeps the badge's mint
           (the colour it had while it still carried its letter) and the other goes grey, so the row
           still answers "where is this going?" at a glance. */
        const int badge_cx = layout.badge_x[i] + ui::kStorageBadge / 2;
        const int badge_cy = layout.badge_y + ui::kStorageBadge / 2;
        if (InstallTargetSelectable()) {
            DrawKeyBadge(surface, m_font, badge_cx, badge_cy, ui::kStorageBadge / 2,
                         i == 0 ? "L" : "R", kHeaderBadge, kOnHeader);
        } else {
            FillRoundedRect(surface, badge_cx - ui::kStorageBadge / 2,
                            badge_cy - ui::kStorageBadge / 2, ui::kStorageBadge, ui::kStorageBadge,
                            ui::kStorageBadge / 2,
                            selected ? kHeaderBadge : kHeaderBadgeOff);
        }
        m_font.Draw(surface, layout.caption_x[i], ui::kStorageCaptionY, kFontSmall,
                    selected ? kOnHeader : kHeaderSubtle, text[i].c_str());
        /* The bar is "how full": the lighter track reads against both the header and the selected
           block's darker face, the filled part is white.  A storage that cannot be read gets a
           bare track and no invented fill. */
        FillRoundedRect(surface, layout.caption_x[i], ui::kStorageBarY, layout.block_w[i],
                        ui::kStorageBarHeight, ui::kStorageBarHeight / 2, kHeaderBadge);
        if (ok[i] && used[i] > 0.0) {
            const int filled =
                static_cast<int>(layout.block_w[i] * (used[i] > 1.0 ? 1.0 : used[i]));
            if (filled > 0) {
                FillRoundedRect(surface, layout.caption_x[i], ui::kStorageBarY, filled,
                                ui::kStorageBarHeight, ui::kStorageBarHeight / 2, kOnHeader);
            }
        }
    }
}


/* The file picker: one card, the directory as rows, the focus as a highlighted row.  Every row's
   hit area comes from the same kRowHeight the list uses, so a tap and the focus can never
   disagree about which row is which. */
/* Going up one level, and picking the focused row: both are "what does the list currently say",
   so they live next to the list instead of in the key handler. */
void App::LeaveDirectory() {
    if (m_file_dir == "/" || m_file_dir.empty()) {
        return;
    }
    std::string parent = m_file_dir;
    while (parent.size() > 1 && parent.back() == '/') {
        parent.pop_back();
    }
    const std::size_t slash = parent.find_last_of('/');
    parent = slash == std::string::npos ? "/" : parent.substr(0, slash + 1);
    if (parent.empty()) {
        parent = "/";
    }
    m_file_dir = parent;
    RefreshFileList();
}

/* A directory opens, an installable file becomes the picked package.  The install itself starts
   from the confirm screen, which is the next screen to be written -- until then the pick is
   recorded and logged so the key map can be tested on hardware. */
void App::PickFocusedFile() {
    const int index = m_file_list.Focus();
    const auto &items = m_file_list.Items();
    if (index < 0 || index >= static_cast<int>(items.size())) {
        return;
    }
    const std::string name = items[static_cast<std::size_t>(index)].name;
    if (name == "..") {
        LeaveDirectory();
        return;
    }
    if (!name.empty() && name.back() == '/') {
        m_file_dir += name.substr(0, name.size() - 1) + "/";
        RefreshFileList();
        return;
    }
    m_picked_file = m_file_dir + name;
    Trace("install: picked %s", m_picked_file.c_str());
    /* A run from the card is the console's, not the page's: nothing here needs a browser, and the
       server (if a source screen had it up) follows this run's own screens instead. */
    m_install_page_drives = false;
    OpenPickedPackage();
}

/* Opens the picked file and parses it, then shows what it is.  Nothing is written yet: the
   confirm screen is the player's chance to say "no, that was the wrong file". */
void App::OpenPickedPackage() {
    m_install_error.clear();
    m_install_status = install::InstallStatus{};
    /* The file list is where this run starts: "back" from the confirm and result screens returns
       here, so the player who picked the wrong file is one press from picking another. */
    m_install_origin = InstallScreen::Files;
    m_file_source.reset();
    m_http_source.reset();
    m_game_install.reset();
    m_cover = ui::Bitmap{};
    if (m_sd == nullptr || m_picked_file.empty()) {
        m_install_error = "no file picked";
        return;
    }
    m_file_source = std::make_unique<install::LocalFileSource>();
    std::uint64_t size = 0;
    std::string error;
    if (!m_file_source->Open(m_sd, m_picked_file, &size, &error)) {
        /* A file that cannot even be opened is a result, not a silent no-op: the player has to
           be told, and log.txt has to say which file and why. */
        m_install_error = error;
        m_file_source.reset();
        Trace("install: cannot open %s: %s", m_picked_file.c_str(), error.c_str());
        m_install_screen = InstallScreen::Result;
        return;
    }
    OpenPackage(*m_file_source, m_picked_file, size, install::TaskSource::LocalFile, std::string());
}

void App::OpenPackage(install::ContainerReader &reader, const std::string &name,
                      std::uint64_t size, install::TaskSource source, const std::string &url) {
    /* A new run, and a new number for it: the page quotes this when it asks the console to start or
       to change the target, so "start the run I am looking at" cannot hit an older one. */
    ++m_run_id;
    m_install_status = install::InstallStatus{};
    m_install_error.clear();
    m_game_install.reset();
    m_cover = ui::Bitmap{};
    install::InstallOptions options; /* the reference implementation's defaults (design section 5) */
    /* The storage the player picked in the header (design section 8.1): it decides where the
       content goes, which is why switching it re-opens the package instead of just changing a
       number on the screen.  The option struct and the engine's own target are the same value --
       the field is what the caller passes on, so the two cannot disagree. */
    options.target = m_install_target;
    m_game_install =
        std::make_unique<install::GameInstall>(reader, name, size, options.target, options);
    /* Where the bytes come from, for the task file: a resume reads it out of that file, and only
       this layer knows which source opened the package (the engine just reads a reader). */
    m_game_install->MutableTask().source = source;
    m_game_install->MutableTask().url = url;
    std::string error;
    if (!m_game_install->Prepare(&error)) {
        m_install_error = error;
        /* Asked before the source is dropped: whether the browser went quiet is the difference
           between "a phone that stopped sending" and "a file that cannot be read", and only the
           source knows. */
        const std::string upload_code =
            m_upload_source != nullptr ? m_upload_source->AbortCode() : std::string();
        std::string cleanup_error;
        m_game_install->CleanupAfterFailure(&cleanup_error);
        m_game_install.reset();
        /* A run that never got as far as being described owes the browser nothing either: drop the
           source so the page can start over (its task id is stale from here on, by design). */
        if (m_upload_source != nullptr) {
            m_upload_source->Abort("the package could not be read");
            m_upload_source.reset();
        }
        /* The raw text is the installer's diagnostic (English, technical) and belongs in
           log.txt; the screen gets a line the player can act on.  A running game is not a dead
           end -- closing it and picking the same file again is one step -- so that case goes
           back to the screen the run started on instead of to a result page that only offers
           "back". */
        Trace("install: %s is not installable: %s", name.c_str(), error.c_str());
        if (error == install::kErrorGameRunning) {
            m_install_screen = m_install_origin;
            return;
        }
        /* A read that failed because the browser went quiet is not a broken file, and saying so
           would send the player to look at a package that is fine (measured 2026-10-16: a phone
           that stops sending during the header read ends up here with whatever the parse made of
           the half-read bytes). */
        if (upload_code == install::kErrorUploadStopped) {
            m_install_error = install::kErrorUploadStopped;
            m_install_screen = InstallScreen::Result;
            return;
        }
        m_install_error = error.find("no installable content") != std::string::npos
                              ? "no-content"
                              : "not-a-package";
        m_install_screen = InstallScreen::Result;
        return;
    }
    m_install_status = m_game_install->Status();
    /* A resume: this run knows where the interrupted one stopped (the task file it was opened
       from), and the engine takes over whatever the console still holds.  It happens *after*
       Prepare, because the contents have to be known before any of them can be matched. */
    if (m_resume_task != nullptr && m_install_status.stage != install::InstallStage::Skipped) {
        std::string resume_error;
        if (m_game_install->AdoptResume(*m_resume_task, &resume_error)) {
            m_install_status = m_game_install->Status();
            m_resumed_bytes = install::DoneBytes(m_game_install->Task());
            Trace("install: resumed from the task file (%s, %llu bytes in)",
                  m_resume_task->name.c_str(),
                  static_cast<unsigned long long>(m_resumed_bytes));
        } else {
            /* Not the same package: start fresh, and leave the old task file alone -- it describes
               a different install, and throwing it away is the player's call. */
            Trace("install: the task file does not describe this package (%s)", resume_error.c_str());
        }
    }
    /* A package the console already has makes the task file moot: nothing will be continued. */
    if (m_resume_task != nullptr && m_install_status.stage == install::InstallStage::Skipped) {
        AbandonPendingInstall();
    }
    m_resume_task = nullptr;
    /* The cover is decoded once per opened package, not per frame: `ns` hands out a JPEG, and the
       confirm page is redrawn on every key and touch event. */
    if (!m_game_install->IconJpeg().empty()) {
        const std::vector<std::uint8_t> &jpeg = m_game_install->IconJpeg();
        if (!ui::DecodeJpeg(jpeg.data(), jpeg.size(), &m_cover)) {
            m_cover = ui::Bitmap{};
            Trace("install: the cover image could not be decoded (%zu bytes)", jpeg.size());
        }
    }
    Trace("install: %s -> %s (%s)", name.c_str(),
          m_install_status.stage == install::InstallStage::Skipped ? "skip" : "confirm",
          m_install_status.detail.c_str());
    /* A package the console already has writes nothing -- but what it leaves behind is still a
       version the agent may need, so the same follow-up runs here as after a real install. */
    RunAgentFollowUp();
    if (m_install_status.stage == install::InstallStage::Skipped) {
        /* The run is over before it started: the browser's part in it is over too, so the page can
           pick the next file instead of being told the console is busy (see ReleaseUploadSource). */
        ReleaseUploadSource("the package was already installed");
    }
    m_install_screen = m_install_status.stage == install::InstallStage::Skipped
                           ? InstallScreen::Result
                           : InstallScreen::Confirm;
}


/* The download source: the address comes from the system keyboard, is probed for Range support
   (which is also where the container's size comes from) and then opens exactly like a file from
   the card.  The two refusals the design names stop *here*, on the address screen: "no network" is
   fixed in the console's settings and a missing Range is fixed by another address, so neither is a
   dead end, and the screen offers the keyboard again. */
void App::StartDownload() {
    std::string address;
    std::string error;
    /* The field starts with the scheme, or with the address that was just refused: "connect to a
       network first" is a one-step fix, and making the player type the whole address again after it
       would be the kind of thing this app is not allowed to do. */
    const std::string initial = m_download_url.empty() ? std::string("http://") : m_download_url;
    if (!util::AskForLine(initial, Tr(i18n::StringId::InstallTypeUrl), &address, &error)) {
        Trace("install: no address was entered (%s)", error.c_str());
        return;
    }
    m_install_page_drives = false; /* typed on the console: not the page's run */
    OpenDownloadPackage(address);
}

/* Opening the package an address names: the Range probe (which is also where the container's size
   comes from) and then the same "what is this" path a card file takes.  Two callers -- the address
   the player just typed, and the re-open after the target storage changed -- and they must not
   drift apart, because the second one has to end up exactly where the first did. */
bool App::OpenDownloadPackage(const std::string &address) {
    std::string error;
    m_install_error.clear();
    m_download_url = address;
    m_http_source = std::make_unique<install::HttpSource>();
    std::uint64_t size = 0;
    if (!m_http_source->Open(address, &size, &error)) {
        m_http_source.reset();
        /* "no-network" and "range-unsupported" are codes the address screen has wording for; the
           log gets whatever curl said. */
        m_install_error = error;
        m_install_screen = InstallScreen::Url;
        Trace("install: %s cannot be downloaded: %s", address.c_str(), error.c_str());
        return false;
    }
    Trace("install: download %s (range probe ok, container %llu bytes)", address.c_str(),
          static_cast<unsigned long long>(size));
    m_install_origin = InstallScreen::Url;
    OpenPackage(*m_http_source, address, size, install::TaskSource::HttpDownload, address);
    return true;
}

/* Keep the screen awake from entering the installer page until the applet exits.  Restoring the flag
   inside the applet did not resume the system's countdown (it either stayed awake or blacked out
   with the stale idle clock), so the deliberate choice is: disable auto-sleep once, never restore
   it here.  The setting is applet-scoped and comes back when ACNH-Manager exits. */
void App::DisableAutoSleep() {
    bool already_disabled = false;
    const Result query = appletIsAutoSleepDisabled(&already_disabled);
    if (R_SUCCEEDED(query) && already_disabled) {
        Trace("install: auto-sleep already disabled");
        return;
    }
    const Result set = appletSetAutoSleepDisabled(true);
    const int was = R_SUCCEEDED(query) ? (already_disabled ? 1 : 0) : -1;
    if (R_FAILED(set)) {
        Trace("install: auto-sleep disable failed (query=0x%x was=%d set=0x%x)",
              static_cast<unsigned>(query), was, static_cast<unsigned>(set));
    } else {
        Trace("install: auto-sleep disabled until app exit (query=0x%x was=%d)",
              static_cast<unsigned>(query), was);
    }
}

/* Runs the whole transfer on this thread.  The engine's steps are bounded (4 MB each), so the
   screen can be redrawn while it works; redrawing every step would spend more time in the
   compositor than in the copy, so a frame every 64 MB is what the player actually needs. */
void App::RunGameInstall() {
    if (m_game_install == nullptr) {
        return;
    }
    m_install_screen = InstallScreen::Progress;
    m_install_error.clear();
    /* From here on the page is answered by this thread only.  The upload source's pump exists so
       that a package being read during the confirm screen can still answer `/upload/next`; during
       the transfer the reader thread kept calling it too, so `/status.json` and `/cover.jpg` were
       built on that thread while this one rewrites the status strings every step.  The race
       corrupted the decoder's state (valid zstd bytes rejected at offset 0) and the app crashed in
       Render the moment it had to allocate (2026-10-20).  The loop below polls the server after
       every step, so the page is still served -- just never concurrently. */
    if (m_upload_source != nullptr) {
        m_upload_source->SetPump(nullptr);
    }
    Render();
    std::string error;
    if (!m_game_install->Begin(&error)) {
        m_install_error = error;
        Trace("install: begin failed: %s", error.c_str());
        m_game_install->Abandon(&error);
        m_install_screen = InstallScreen::Result;
        Render();
        return;
    }
    while (m_game_install->Step(&error)) {
        m_install_status = m_game_install->Status();
        /* The transfer owns this thread for minutes at a time, so the browser page is answered from
           inside its loop as well: that is what lets a phone watch progress while the bytes move. */
        PollInstallerServer();
        /* Redraw for every step instead of every sixteenth: a step is one 4 MiB range, which took
           406 ms on average on the gateway path (33.8 s for 83 steps, measured 2026-10-16), so this
           is about two frames a second -- and the transfer's own timing is 99.7% network wait, so
           the drawing has nothing to steal from.  The bar then moves while the bytes move. */
        Render();
    }
    m_install_status = m_game_install->Status();
    if (m_install_status.stage == install::InstallStage::Failed) {
        m_install_error = m_install_status.error;
        /* The same distinction as at the open: a transfer that stopped because the browser went
           quiet gets the sentence the player can act on, not the byte it happened to be reading. */
        if (m_upload_source != nullptr &&
            m_upload_source->AbortCode() == install::kErrorUploadStopped) {
            m_install_error = install::kErrorUploadStopped;
        }
        /* The failure has to reach log.txt in full: the screen may wrap or clip it, and a
           diagnostic that only exists on a 1280-pixel card is not a diagnostic. */
        Trace("install: transfer failed: %s", m_install_error.c_str());
        /* A transfer that failed is *not* a cancelled install: the bytes that arrived are still in
           their placeholders, and the task file still describes them, so the player can pick this up
           again instead of watching a few gigabytes go to waste (design section 10).  Only the
           placeholders that merely served reading (meta/control) go away with the package. */
        m_game_install->CleanupAfterFailure(&error);
        /* Its next `/upload/begin` is how the player carries on (the task file kept a line above
           says what is still missing). */
        ReleaseUploadSource("the run failed");
        /* The task file (and the bytes it describes) is still there: the next entry into the install
           page offers to continue, and the result page says so below. */
        RefreshPendingTask();
        m_install_screen = InstallScreen::Result;
        Render();
        return;
    }
    /* Everything is in: from here the install cannot be cancelled, and the screen says so. */
    m_install_screen = InstallScreen::Progress;
    Render();
    /* The outcome and the stage are logged *before* anything else touches the install: the run
       that reported success while the console kept the old version left no trace at all, which
       meant guessing instead of reading. */
    const bool finished = m_game_install->Finish(&error);
    {
        const auto &status = m_game_install->Status();
        Trace("install: finish returned %d stage=%d%s%s", finished ? 1 : 0,
              static_cast<int>(status.stage), error.empty() ? "" : " error=", error.c_str());
    }
    if (!finished) {
        m_install_error = error;
        Trace("install: finish failed: %s", error.c_str());
        m_game_install->Abandon(&error);
    }
    m_install_status = m_game_install->Status();
    Trace("install: finished stage=%d%s%s", static_cast<int>(m_install_status.stage),
          m_install_error.empty() ? "" : " error=", m_install_error.c_str());
    /* The package is on the console: if what it leaves behind is a version the agent supports,
       the agent goes in now, in the same run (RunAgentFollowUp).  It draws the same progress card
       for its own step, and the result screen below replaces it when it is done. */
    RunAgentFollowUp();
    m_install_screen = InstallScreen::Result;
    /* The run may have finished the task the page was told about: re-read what is left on the card
       so `/status.json` (and the page's "unfinished install" card) does not keep offering a task
       the engine just deleted. */
    RefreshPendingTask();
    /* The install is on the console: the browser is done offering bytes for it, and holding its
       connection would only make the next pick look like a second install. */
    ReleaseUploadSource("the install finished");
    /* The package stays in hand for as long as the result page is up: that page names what was
       installed and, when the run failed, quotes the reason the installer gave.  Dropping it here
       is how a failed finish ended up labelled "this file is not an install package" on a package
       that had just been parsed, confirmed and transferred.  LeaveGameInstall() releases it. */
    Collect(); /* the home screen's verdict may have changed: the gate watches the game version */
    Render();
}

void App::LeaveGameInstall() {
    /* The modal page is what the server existed for, so it goes down with it (design section 3). */
    StopInstallerServer();
    m_install_page_drives = false; /* a fresh entry into the install page starts from scratch */
    m_install_returns_to_launcher = false;
    LeaveInstallScreen();
    m_page = m_game_install_return;
}

void App::LeaveInstallScreen() {
    if (m_game_install != nullptr) {
        const install::InstallStage stage = m_game_install->Status().stage;
        /* A package that was never installed owes the console the placeholders the CNMT/NACP
           read created; a run that got as far as registering owns nothing to take back (the
           content and the record are the player's), which is why the stage is checked before
           `Abandon` -- that call also rolls a registered run back. */
        if (m_game_install->KeepsTask()) {
            /* The run failed *after* its task file went down, so its placeholders are what a
               "continue" would pick up: leaving this screen is not a cancel (design section 10).
               Only the placeholders that merely served reading go away; the next run re-creates
               them.  Dropping everything here is exactly how a resumable failure became
               unrecoverable -- measured on hardware. */
            std::string ignored;
            m_game_install->CleanupAfterFailure(&ignored);
        } else if (stage != install::InstallStage::Done && stage != install::InstallStage::Skipped) {
            std::string ignored;
            m_game_install->Abandon(&ignored);
        }
        Trace("install: left the screen (stage=%d, task kept=%d)", static_cast<int>(stage),
              m_game_install->KeepsTask() ? 1 : 0);
    }
    m_game_install.reset();
    m_file_source.reset();
    m_http_source.reset();
    m_memory_source.reset();
    /* The upload source too: it is holding a browser's connection and the page's task id, and
       leaving the screen is exactly when that should end (the page says "the console is offline"
       and a new run starts from a fresh `POST /upload/begin`). */
    if (m_upload_source != nullptr) {
        m_upload_source->Abort("the console left the install page");
        m_upload_source.reset();
    }
    m_cover = ui::Bitmap{};
    m_install_error.clear();
}

void App::InstallStepBack() {
    switch (m_install_screen) {
        case InstallScreen::Confirm:
        case InstallScreen::Result:
            if (m_install_returns_to_launcher) {
                LeaveGameInstall();
                break;
            }
            /* Back to where the run in hand started: the package (and the placeholders the
               CNMT/NACP read created for it) is dropped on the way. */
            LeaveInstallScreen();
            m_install_screen = m_install_origin;
            break;
        case InstallScreen::Files:
            /* The list keeps its position; picking another file is one press away. */
            m_install_screen = InstallScreen::Choose;
            break;
        case InstallScreen::Url:
        case InstallScreen::Upload:
            /* The two source screens are where their runs start, so their "back" is the picker
               above them -- and from there the next B leaves the installer.  Both clear the same
               three things: the last reason, the address that caused it, and the download source
               that may still hold a socket. */
            m_http_source.reset();
            m_download_url.clear();
            m_install_error.clear();
            m_install_screen = InstallScreen::Choose;
            break;
        case InstallScreen::Choose:
            LeaveGameInstall();
            break;
        case InstallScreen::Progress:
        default:
            return; /* the engine owns the thread: nothing may leave this screen */
    }
    m_focus = FirstEnabled(m_actions);
}

/* Where this run writes.  The choice is a preference (settings.json) and it is also a fact about
   the run in hand: reading a package's CNMT and control data leaves placeholders behind *on the
   target storage* (they are what the console mounts to hand us the meta), so switching the target
   while a package is open means dropping that run and opening the same package again on the new
   storage -- the local file is re-parsed, a download re-reads its header and CNMT.  The design
   keeps the selector off the progress and result screens (section 8.1): there the target is
   locked in, or the run is already over. */
void App::SetInstallTarget(install::TargetStorage target) {
    if (m_install_target == target) {
        return; /* already selected: L on the selected block must not re-read the package */
    }
    if (!InstallTargetSelectable()) {
        return; /* the target is committed, or the run is finished */
    }
    m_install_target = target;
    Trace("install: target=%s", target == install::TargetStorage::Nand ? "nand" : "sd");
    SaveSettings();
    const bool upload = m_upload_source != nullptr;
    if (m_game_install == nullptr && !upload) {
        return; /* nothing is open yet: the next open already uses the new storage */
    }
    const bool from_url = m_http_source != nullptr;
    const std::string address = m_download_url;
    LeaveInstallScreen();
    if (upload) {
        /* An upload's bytes come from the browser and are already in placeholders on the old
           target, so its package cannot be re-opened the way a file or an address can: the run
           starts over and the screen goes back to where the file is picked.  `LeaveInstallScreen`
           has already aborted the source, which is what tells the page its task id is stale and
           makes it ask for the file again. */
        Trace("install: the target changed during an upload; the page starts over");
        m_install_screen = InstallScreen::Upload;
        return;
    }
    if (from_url) {
        OpenDownloadPackage(address);
    } else {
        OpenPickedPackage();
    }
}

/* The install the last run left behind.  Reading it is cheap (one small JSON), so it happens
   wherever the answer can change: when the install page opens, and when a run ends.  A file we
   cannot parse is not a pending install -- it is deleted, because a task nobody can act on would
   only hold space forever. */
void App::RefreshPendingTask() {
    m_have_pending_task = false;
    m_pending_restarts = false;
    if (m_sd == nullptr) {
        return;
    }
    std::string text;
    bool found = false;
    std::string error;
    if (!install::ReadTextFile(*m_sd, kTaskPath, &text, &found, &error)) {
        Trace("install: the task file could not be read: %s", error.c_str());
        return;
    }
    if (!found) {
        return;
    }
    install::InstallTask parsed;
    std::string parse_error;
    if (!install::ParseTask(text, &parsed, &parse_error)) {
        Trace("install: the task file is unusable (%s); removing it", parse_error.c_str());
        std::string ignored;
        install::RemoveFileIfPresent(*m_sd, kTaskPath, &ignored);
        return;
    }
    /* A task whose source cannot be opened again is not actionable either -- the browser upload of
       1.1.0 does not exist yet (M4), and a local file the player deleted is gone.  Both cases keep
       the file (the player may put it back) but must not offer a "continue" that cannot work. */
    m_pending_task = std::move(parsed);
    m_have_pending_task = true;
    /* What the *file* claims, and then what the console really holds: a reboot clears placeholders,
       so a resumed install would start over while the file still says "1.0 GB in".  The screens
       show the live numbers, and say so when the two disagree. */
    const std::uint64_t claimed = install::DoneBytes(m_pending_task);
    m_pending_live_bytes = 0;
    m_pending_live_reserved = 0;
    m_pending_restarts = false;
    std::uint64_t reserved_from_file = 0;
    for (const auto &content : m_pending_task.contents) {
        if (!content.placeholder_id.empty()) {
            reserved_from_file += content.size;
        }
    }
    install::ContentInstaller installer(m_pending_task.target, install::InstallOptions{});
    std::string probe_error;
    bool probed = false;
    if (installer.Open(&probe_error)) {
        probed = true;
        for (const auto &content : m_pending_task.contents) {
            if (content.placeholder_id.empty()) {
                continue;
            }
            bool exists = false;
            if (!installer.HasPlaceHolder(content.placeholder_id, &exists, &probe_error) || !exists) {
                /* The file says these bytes are in, and the console says the placeholder is gone:
                   the one case where "continue" cannot continue (a reboot clears placeholders).
                   A content that is *complete* in the file does not count: the meta and control
                   contents are read through a placeholder of their own, which is dropped after the
                   read, and the next run re-creates it anyway -- warning about those would put the
                   note on every failed download. */
                if (content.written > 0 && content.written < content.size) {
                    m_pending_restarts = true;
                }
                continue;
            }
            m_pending_live_bytes += content.written;
            m_pending_live_reserved += content.size;
        }
    } else {
        Trace("install: could not ask the console about the unfinished install (%s); showing the "
              "file's numbers",
              probe_error.c_str());
        m_pending_live_bytes = claimed;
        m_pending_live_reserved = reserved_from_file;
    }
    if (!probed) {
        m_pending_restarts = false; /* the console could not be asked: do not claim it is gone */
    }
    Trace("install: unfinished install found (%s, %zu contents, file says %llu bytes, console holds "
          "%llu)",
          m_pending_task.name.c_str(), m_pending_task.contents.size(),
          static_cast<unsigned long long>(claimed),
          static_cast<unsigned long long>(m_pending_live_bytes));
}

/* Continue: the same source, opened exactly the way a fresh run would open it.  What makes it a
   resume is `m_resume_task`, which OpenPackage hands to the engine after the package has been read
   (the engine then adopts whatever the console still holds). */
void App::ResumePendingInstall() {
    if (!m_have_pending_task) {
        m_install_screen = InstallScreen::Choose;
        return;
    }
    m_resume_task = &m_pending_task;
    switch (m_pending_task.source) {
        case install::TaskSource::LocalFile:
            m_picked_file = m_pending_task.name;
            OpenPickedPackage();
            return;
        case install::TaskSource::HttpDownload:
            if (m_pending_task.url.empty()) {
                Trace("install: the task file names no address; dropping the resume");
                break;
            }
            OpenDownloadPackage(m_pending_task.url);
            return;
        case install::TaskSource::HttpUpload:
            /* An upload's bytes come from a browser, so "continue" cannot fetch anything by itself:
               the console shows the address and waits, and the run picks up when the page offers the
               same file again (see the adoption in PollInstallerServer).  `m_resume_task` is left
               as it is -- the adoption sets it once the browser has actually sent something, so a
               player who walks away with the page still open does not leave a resume armed. */
            Trace("install: the task came from a browser upload; waiting for the page to offer it");
            m_resume_task = nullptr;
            m_install_error.clear();
            m_install_screen = InstallScreen::Upload;
            return;
        case install::TaskSource::None:
        default:
            break;
    }
    m_resume_task = nullptr;
    m_install_error = Tr(i18n::StringId::PendingUnresumable);
    m_install_screen = InstallScreen::Pending;
}

/* Give up: delete what the interrupted run created.  Only our own placeholder ids go away -- the
   ids in the task file, nothing else (no CleanupAllPlaceHolder, which would empty other tools'
   placeholders too).  Contents the interrupted run had already *registered* are not deleted here:
   a crash between Register and the record is the one case the installer's own "install again"
   repair path handles, and deleting a title the console already had is not ours to do. */
void App::AbandonPendingInstall() {
    if (!m_have_pending_task) {
        m_install_screen = InstallScreen::Choose;
        return;
    }
    std::vector<install::Placeholder> placeholders;
    for (const auto &content : m_pending_task.contents) {
        if (content.content_id.empty() || content.placeholder_id.empty()) {
            continue;
        }
        placeholders.push_back({content.content_id, content.placeholder_id});
    }
    bool ok = true;
    std::string error;
    if (!placeholders.empty()) {
        install::ContentInstaller installer(m_pending_task.target, install::InstallOptions{});
        if (!installer.Open(&error)) {
            ok = false;
        } else if (!installer.DropPlaceholders(placeholders, &error)) {
            ok = false;
        }
    }
    if (ok) {
        std::string remove_error;
        if (!install::RemoveFileIfPresent(*m_sd, kTaskPath, &remove_error)) {
            ok = false;
            error = remove_error;
        }
    }
    Trace("install: gave up the unfinished install (%zu placeholders) ok=%d%s%s", placeholders.size(),
          ok ? 1 : 0, error.empty() ? "" : " error=", error.c_str());
    m_install_error = ok ? std::string() : error;
    RefreshPendingTask();
    m_install_screen = m_have_pending_task ? InstallScreen::Pending : InstallScreen::Choose;
}

bool App::InstallTargetSelectable() const {
    if (m_page != Page::GameInstall) {
        return false;
    }
    switch (m_install_screen) {
        case InstallScreen::Choose:
        case InstallScreen::Files:
        case InstallScreen::Url:
        case InstallScreen::Upload:
        case InstallScreen::Confirm:
            return true;
        case InstallScreen::Progress:
        case InstallScreen::Result:
        default:
            return false;
    }
}

bool App::AgentInstalled() const {
    /* "NeedsInstall" is exactly "this console has no agent yet" and "GameMissing" is a console with
       no game to attach one to; everything else counts as present here -- including a half-written
       agent, which the home screen offers to repair rather than to install from scratch. */
    return m_have_state && m_home_kind != HomeKind::NeedsInstall &&
           m_home_kind != HomeKind::GameMissing;
}

/* "Animal Crossing: New Horizons 3.0.3 (Update)" -- what the run in hand is, in one line.  The
   confirm page, the progress page and the result page all name it the same way, because a player
   who looks away during a four-gigabyte transfer has to be able to tell what is being written
   without going back. */
std::string App::PackageSummary() const {
    if (m_game_install == nullptr) {
        return std::string();
    }
    const install::PackageInfo &package = m_game_install->Package();
    i18n::StringId kind = i18n::StringId::TypeUpdate;
    switch (package.cnmt.type) {
        case install::MetaType::Application:
            kind = i18n::StringId::TypeBase;
            break;
        case install::MetaType::AddOnContent:
            kind = i18n::StringId::TypeDlc;
            break;
        default:
            break; /* a patch, a data patch, or a kind the manifest does not name */
    }
    std::string summary = m_install_status.game_name.empty() ? package.cnmt.title_id
                                                             : m_install_status.game_name;
    if (!m_install_status.game_version.empty()) {
        summary += " " + m_install_status.game_version;
    }
    summary += "(" + std::string(Tr(kind)) + ")";
    return summary;
}

/* The progress screen: how far along by *bytes*, because one content in a real update is several
   GB and the others are kilobytes -- "3 of 5 contents" would sit at 20% until the very end. */
void App::RenderInstallProgress(Surface surface) {
    m_actions.clear();
    const int card_x = 200;
    const int card_top = kHeaderHeight + 74;
    const int card_width = surface.width - card_x * 2;
    const int card_height = 330;
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);
    /* The same cover the confirm page drew, for the same reason: a transfer that takes minutes
       must not leave the player guessing which game this is.  It also takes the room it needs out
       of the card first, so no line here can run under it. */
    const int text_width = DrawPackageCover(surface, card_x, card_top, card_width, card_height);
    const int text_x = card_x + 36;

    /* The card serves two steps of one run: the package's own transfer (bytes, contents) and, right
       after it, the agent's install (files).  Same shape and same colours, but honest numbers --
       the agent step has no byte counts of its own, so its counter and bar follow the files it is
       writing, and the GB line (which would read "0.00 / 0.00 GB") is not drawn at all. */
    const bool agent_step = m_install_follow_up;
    const bool finalizing = !agent_step && m_install_status.stage == install::InstallStage::Finalizing;
    const char *stage = Tr(agent_step   ? i18n::StringId::ProgressInstalling
                                      : (finalizing ? i18n::StringId::ProgressFinalize
                                                    : i18n::StringId::ProgressReceive));

    /* What is being installed, in the words the confirm page used -- the one line the old card was
       missing: "3 of 5 contents" said nothing about *what* those contents are for. */
    m_font.Draw(surface, text_x, card_top + 26, kFontBody, kText,
                m_font.Fit(PackageSummary(), kFontBody, text_width, 1).c_str());
    m_font.DrawBold(surface, text_x, card_top + 62, kFontHeading, kText, stage);

    char counter[64];
    /* "Content N of M" names the one being written, so the package step counts the current one --
       `done_contents` counts *finished* contents, which printed "0 of 1" for the whole of a
       single-content package (an add-on), reading like nothing had started.  The engine's own
       counter for the agent step is already the file in hand. */
    const int counter_done =
        agent_step ? m_progress_index
                   : install::CurrentContentNumber(m_install_status.done_contents,
                                                   m_install_status.total_contents);
    const int counter_total = agent_step ? m_progress_total : m_install_status.total_contents;
    std::snprintf(counter, sizeof(counter), Tr(i18n::StringId::ProgressContents), counter_done,
                  counter_total);
    m_font.Draw(surface, text_x, card_top + 106, kFontSmall, kSubtle, counter);

    const std::string &current_name =
        agent_step ? m_progress_step : m_install_status.current_name;
    m_font.Draw(surface, text_x, card_top + 138, kFontBody, kText,
                m_font.Fit(current_name, kFontBody, text_width, 1).c_str());

    /* The bar keeps to the text's column: the percent sits to the right of it, still clear of the
       cover. */
    const int bar_x = text_x;
    const int bar_y = card_top + 194;
    const int bar_width = text_width - 130;
    constexpr int kBarHeight = 16;
    FillRoundedRect(surface, bar_x, bar_y, bar_width, kBarHeight, kBarHeight / 2, kCardDisabled);
    std::uint64_t total = m_install_status.total_bytes;
    std::uint64_t done = m_install_status.done_bytes;
    if (agent_step) {
        total = static_cast<std::uint64_t>(m_progress_total > 0 ? m_progress_total : 0);
        done = static_cast<std::uint64_t>(m_progress_index > 0 ? m_progress_index : 0);
    }
    if (total > 0) {
        const double ratio = static_cast<double>(done) / static_cast<double>(total);
        const int filled = static_cast<int>(bar_width * (ratio > 1.0 ? 1.0 : ratio));
        if (filled > 0) {
            FillRoundedRect(surface, bar_x, bar_y, filled, kBarHeight, kBarHeight / 2, kHeader);
        }
    }
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%",
                  total > 0 ? static_cast<int>(done * 100 / total) : 0);
    m_font.DrawBold(surface, bar_x + bar_width + 18, bar_y - 10, kFontHeading, kText, percent);

    if (!agent_step) {
        std::string detail = util::FormatSize(done) + " / " + util::FormatSize(total);
        const std::string rate = InstallRateText();
        if (!rate.empty()) {
            detail += " · " + rate;
        }
        const std::string eta = InstallEtaText();
        if (!eta.empty()) {
            detail += " · " + eta;
        }
        m_font.Draw(surface, text_x, card_top + 236, kFontSmall, kSubtle,
                    m_font.Fit(detail, kFontSmall, text_width, 1).c_str());
    }
    if (finalizing) {
        /* Says the one thing the numbers cannot: this part cannot be cancelled. */
        m_font.Draw(surface, text_x, card_top + 268, kFontSmall, kWarn,
                    Tr(i18n::StringId::ProgressFinalize));
    }
}

/* The installer's web server (M4).  Lifecycle first, because it is a resource: a listening socket
   that nobody asked for is a port somebody else could have used, so it exists exactly while there
   is something for it to do (design section 3). */
std::string App::InstallerAddressText() const {
    if (m_http_port == 0) {
        return std::string();
    }
    std::string ip;
    std::string error;
    if (!install::CurrentIpAddressText(&ip, &error)) {
        return std::string();
    }
    std::string address =
        "http://" + ip + ":" + std::to_string(static_cast<unsigned>(m_http_port));
    /* The QR code and the printed address must be the same URL.  Development builds (DEV_AIDS or a
       +devaids stamp) open the debug trace by default; release builds keep the plain address. */
    bool development = false;
#if ACNH_DEV_AIDS
    development = true;
#endif
    if (std::string(kBuildStamp).find("+devaids") != std::string::npos) {
        development = true;
    }
    if (development) {
        address += "/?debug=1";
    }
    return address;
}

/* The two screens the page belongs to, and the runs they start.  `m_install_origin` is what says
   where the run in hand came from: a file picked from the card (Files), the console's keyboard or
   the page (Url), or the page's upload (Upload).  Only the last two need a page watching. */
bool App::InstallerServerNeeded() const {
    if (m_page != Page::GameInstall) {
        return false;
    }
    if (m_install_screen == InstallScreen::Url || m_install_screen == InstallScreen::Upload) {
        return true;
    }
    /* The screens a run from those two goes through -- including the result screen, which is also
       where a run that failed *before* it had a package ends up (`m_game_install` is gone by then,
       so asking "is a run in hand" is not enough): the page watching that run has to be able to
       read the failure, or it shows "offline" over a console that is answering nothing (measured
       2026-10-16, right after a browser upload stalled). */
    switch (m_install_screen) {
        case InstallScreen::Confirm:
        case InstallScreen::Progress:
        case InstallScreen::Result:
            break;
        default:
            return false; /* pick a source, pick a file, or an unfinished install: no page involved */
    }
    const bool from_a_source_screen = m_install_origin == InstallScreen::Url ||
                                      m_install_origin == InstallScreen::Upload;
    return from_a_source_screen || m_install_page_drives;
}

void App::SyncInstallerServer() {
    if (InstallerServerNeeded()) {
        StartInstallerServer();
    } else {
        StopInstallerServer();
    }
}

void App::StartInstallerServer() {
    if (m_http_server.Listening()) {
        return;
    }
    m_http_server.SetLog(m_log);
    m_http_server.SetHandler([this](const net::HttpRequest &request) { return ServeHttp(request); });
    std::uint16_t port = 0;
    std::string error;
    if (!m_http_server.Start(net::kHttpDefaultPort, &port, &error)) {
        /* No page for this run.  The address screen will show nothing (with the address printed as
           unavailable) and the log keeps the reason -- and the two console-side ways in (typing the
           address, picking a file from the card) are unaffected. */
        Trace("http: the browser page could not be served: %s", error.c_str());
        return;
    }
    m_http_port = port;
    Trace("http: page ready at %s", InstallerAddressText().c_str());
}

void App::StopInstallerServer() {
    if (!m_http_server.Listening()) {
        return;
    }
    m_http_server.Stop();
    m_http_port = 0;
    m_http_pending_url.clear();
    Trace("http: the page server stopped");
}

void App::RefreshStatusStorage() {
    StorageBarTexts(m_status_storage_text, m_status_storage_ok, m_status_storage_used);
}

/* The line the confirm card puts under its rows: whether the agent can follow this package.  Three
   cases, not two -- our game with a supported version (it will be installed), our game with one the
   agent does not handle (it has to say which version it does), and everything else (an add-on, a data
   patch, another game: nothing to say).  The console's own card and the page both read this. */
std::string App::PackageAgentNote(bool *warn) const {
    if (warn != nullptr) {
        *warn = false;
    }
    if (m_game_install == nullptr) {
        return std::string();
    }
    const install::PackageInfo &package = m_game_install->Package();
    if (!TitleIsOurs(package.cnmt.title_id) ||
        !install::MetaTypeCarriesGameVersion(package.cnmt.type)) {
        return std::string();
    }
    if (AgentSupportedFor(package.cnmt.title_id, package.cnmt.version)) {
        return Tr(i18n::StringId::ConfirmAgentNote);
    }
    if (warn != nullptr) {
        *warn = true;
    }
    char note[192];
    std::snprintf(note, sizeof(note), Tr(i18n::StringId::InstallAgentUnsupported),
                  AgentSupportedVersionsFor(package.cnmt.title_id).c_str());
    return std::string(note);
}

std::string App::InstallRateText() const {
    /* Only while bytes are moving: once the transfer is in its closing steps the last number is
       history, and printing it next to a full bar reads like the run is still downloading. */
    if (m_install_status.stage != install::InstallStage::Fetching ||
        m_install_status.bytes_per_second == 0) {
        return std::string();
    }
    return i18n::Format(i18n::StringId::ProgressRate,
                        util::FormatSize(m_install_status.bytes_per_second).c_str());
}

std::string App::InstallEtaText() const {
    const std::uint64_t seconds = m_install_status.eta_seconds;
    /* Same gate as the speed: a number left over from the last fetch is history once the run has
       moved on (the transfer's closing steps have no meaningful "time left"). */
    if (m_install_status.stage != install::InstallStage::Fetching || seconds == 0) {
        return std::string();
    }
    if (seconds < 60) {
        return i18n::Format(i18n::StringId::ProgressEtaSeconds, static_cast<int>(seconds));
    }
    /* Rounded up to the next minute: a card that says "8 minutes" and finishes in seven is a
       better card than one that keeps saying "1 minute" for ninety seconds. */
    const std::uint64_t minutes = (seconds + 59) / 60;
    if (minutes < 60) {
        return i18n::Format(i18n::StringId::ProgressEtaMinutes, static_cast<int>(minutes));
    }
    if (minutes % 60 == 0) {
        /* A whole hour reads better without "and 0 minutes". */
        return i18n::Format(i18n::StringId::ProgressEtaHoursOnly,
                            static_cast<int>(minutes / 60));
    }
    /* "12 h 23 min" is the longest form the progress line ever has to hold; at a phone's width it
       wrapped the byte line.  One decimal is just as useful for an estimate and keeps the sentence
       on one line ("12.4 h"). */
    char hours[16];
    std::snprintf(hours, sizeof(hours), "%.1f", static_cast<double>(seconds) / 3600.0);
    return i18n::Format(i18n::StringId::ProgressEtaHours, hours);
}

void App::PollInstallerServer() {
    if (!m_http_server.Listening()) {
        return;
    }
    /* The two storage blocks the page mirrors in its header: same numbers the console draws, asked
       for once a second rather than once per request. */
    const std::uint64_t now_ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
    if (now_ms - m_status_storage_ms > 1000) {
        m_status_storage_ms = now_ms;
        RefreshStatusStorage();
    }
    ServiceInstallerPage();
    /* `POST /start` is exactly the console's `A` on the confirm screen: the same function, so the
       two devices cannot drift apart (design section 3).  Taken here, not in the socket pass,
       because it owns this thread for the whole transfer. */
    if (m_http_pending_start) {
        m_http_pending_start = false;
        if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Confirm &&
            m_game_install != nullptr) {
            Trace("install: the page pressed start");
            RunGameInstall();
            return;
        }
    }
    /* The page's Ⓑ on the result card: the same step as the console's key, so the next package can
       be picked from the phone. */
    if (m_http_pending_back) {
        m_http_pending_back = false;
        if (m_page == Page::GameInstall &&
            (m_install_screen == InstallScreen::Result ||
             m_install_screen == InstallScreen::Confirm)) {
            Trace("install: the page went back");
            InstallStepBack();
            return;
        }
    }
    /* `POST /target` goes through the same selector the header's blocks use, which re-opens the
       package on the new storage (design section 8.1). */
    if (m_http_pending_target) {
        m_http_pending_target = false;
        Trace("install: the page picked the target storage");
        SetInstallTarget(m_http_target);
    }
    /* A file the browser picked becomes a package from an upload source: Prepare reads its first
       bytes through the page (which is why the source can pump the server while it waits). */
    if (m_http_pending_upload) {
        m_http_pending_upload = false;
        /* Whatever screen the console happens to be on, this run is the page's: it offered the file,
           so it keeps its server until the run is over (see InstallerServerNeeded). */
        m_install_page_drives = true;
        const std::string name = m_http_upload_name;
        const std::uint64_t size = m_http_upload_size;
        m_http_upload_name.clear();
        m_http_upload_size = 0;
        /* A file the page picks while an unfinished *upload* is still on the card is nearly always
           that same file: the page tells the player to re-open it and pick it again, and resuming is
           the whole point of keeping the task.  The engine decides whether it really is the same
           package (the task file's container id, size and contents); a different file simply starts
           a fresh run, exactly as it would if the task had never been there. */
        if (m_have_pending_task && m_pending_task.source == install::TaskSource::HttpUpload) {
            m_resume_task = &m_pending_task;
            Trace("install: the page's file is checked against the unfinished upload");
        }
        m_upload_source = std::make_unique<install::HttpUploadSource>();
        std::string error;
        if (!m_upload_source->Begin(name, size, m_http_upload_task_id, &error)) {
            Trace("install: the page's file cannot be used: %s", error.c_str());
            m_upload_source.reset();
        } else {
            /* The reader serves the page while it waits: without this, reading the package would
               deadlock against the page's own `/upload/next` (see the source's note). */
            m_upload_source->SetPump([this] { ServiceInstallerPage(); });
            m_upload_source->SetRequestsProbe([this] { return m_http_server.RequestsServed(); });
            m_upload_source->SetLog(m_log);
            Trace("install: the page offers %s (%llu bytes, task %s)", name.c_str(),
                  static_cast<unsigned long long>(size), m_upload_source->TaskId().c_str());
            /* Prepare() reads the package on this thread, and while the browser is fetching those
               pieces the loop would otherwise freeze on the source screen for up to fifty seconds.
               The wait pump redraws the source screen (with the countdown) from inside the read; it
               is cleared before the transfer starts, whose reads run on the reader thread. */
            m_upload_reading = true;
            m_upload_source->SetWaitPump([this] {
                if (m_upload_reading) {
                    Render();
                }
            });
            OpenPackage(*m_upload_source, name, size, install::TaskSource::HttpUpload, std::string());
            m_upload_reading = false;
            if (m_upload_source != nullptr) {
                m_upload_source->SetWaitPump(nullptr);
            }
        }
    }
    /* An address the *browser* submitted becomes an ordinary download run: the same code path the
       console's own keyboard uses, which is the point of this entry (design section 14, item 28). */
    if (!m_http_pending_url.empty()) {
        const std::string address = m_http_pending_url;
        m_http_pending_url.clear();
        m_install_page_drives = true; /* the page submitted it: same rule as above */
        Trace("install: the page submitted an address; opening it as a download");
        m_install_error.clear();
        OpenDownloadPackage(address);
    }
}

/* One pass of the page's server plus the hand-off it produced: a part the browser posted goes to
   the upload source, which may be waiting for it on this thread (the package being read) or on the
   transfer's fetch thread.  Two callers use this as-is (the frame loop and the transfer loop), and a
   third -- the upload source itself, while it waits -- because otherwise reading the package would
   deadlock against the very request that carries its bytes. */
/* The browser's part in a run ends when the run does: the source holds one of the page's
   connections and the task id it quotes, and the page's next `POST /upload/begin` is how the player
   starts or continues.  A source left over answers that with a 409 "busy" -- which is what a page
   got after a skipped install, and what the transfer failure path had to work around by hand
   (2026-10-16). */
void App::ReleaseUploadSource(const char *why) {
    if (m_upload_source == nullptr) {
        return;
    }
    Trace("install: the browser is done with this run (%s)", why);
    m_upload_source->Abort(why);
    m_upload_source.reset();
    m_http_pending_upload = false;
    m_http_upload_name.clear();
    m_http_upload_size = 0;
}

void App::ServiceInstallerPage() {
    m_http_server.Poll();
    net::HttpHandoff handoff;
    if (m_http_server.TakeHandoff(&handoff)) {
        if (m_upload_source == nullptr) {
            /* A part arrived after the run let go of its source: nobody will read these bytes, and a
               connection that is neither answered nor closed stays in the browser's pool until it
               runs out of them (six per host, 2026-10-16).  Close it and say so. */
            Trace("http: a part arrived with no upload source; closing it");
            close(handoff.fd);
            return;
        }
        /* One line per part: offset, length, and the first bytes, because "the console refused the
           package" and "the bytes arrived shifted" look the same from the screens. */
        if (m_log != nullptr) {
            /* `Block`, not `Trace`: the package read runs inside the engine's log burst, which
               buffers every line until it ends -- and these are exactly the lines needed while the
               read is stuck. */
            char line[192];
            std::snprintf(line, sizeof(line),
                          "http: part at %llu (%zu bytes, %zu already read, first=%02x%02x%02x%02x)\n",
                          static_cast<unsigned long long>(handoff.offset), handoff.length,
                          handoff.prefix.size(), handoff.prefix.size() > 0 ? handoff.prefix[0] : 0,
                          handoff.prefix.size() > 1 ? handoff.prefix[1] : 0,
                          handoff.prefix.size() > 2 ? handoff.prefix[2] : 0,
                          handoff.prefix.size() > 3 ? handoff.prefix[3] : 0);
            m_log->Block(line);
        }
        m_upload_source->AttachPart(handoff.fd, handoff.offset, handoff.length,
                                    std::move(handoff.prefix));
    }
}

/* The one line under the result card's title.  Every branch is wording, not state: which of them
   applies is decided here, and the browser page's status payload sends the same string instead of
   keeping a second copy of the rules. */
std::string App::InstallOutcomeLine() const {
    const install::InstallStatus &status = m_install_status;
    switch (status.stage) {
        case install::InstallStage::Skipped:
            /* The console's own name for the version it has -- never the title version number,
               which is Nintendo's build id (3.0.3 is 0x220000, i.e. "34.0.0" if read as one).  When
               the console has no name for it, the sentence leaves the number out rather than
               inventing one. */
            if (status.installed_version > status.package_version) {
                if (status.installed_version_text.empty()) {
                    return Tr(i18n::StringId::ResultNewerVersionPlain);
                }
                return i18n::Format(i18n::StringId::ResultNewerVersionDetail,
                                    status.installed_version_text.c_str());
            }
            return Tr(i18n::StringId::ResultSameVersionDetail);
        case install::InstallStage::Done: {
            return i18n::Format(i18n::StringId::ResultWroteLine,
                                Tr(m_install_target == install::TargetStorage::Nand
                                       ? i18n::StringId::StorageSystem
                                       : i18n::StringId::StorageSdCard),
                                status.total_contents,
                                util::FormatSize(status.total_bytes).c_str());
        }
        default:
            /* The one failure whose code the screens word themselves: the browser stopped sending.
               Everything else is quoted the way the installer said it, because a canned sentence
               on a failure that came from somewhere else sends the player to the wrong place --
               and printing the *code* is worse than either (a hardware screen read "upload-stopped"
               next to the title, 2026-10-16). */
            if (m_install_error == install::kErrorUploadStopped) {
                return Tr(i18n::StringId::InstallUploadStopped);
            }
            return m_install_error.empty() ? std::string("the install did not finish")
                                           : m_install_error;
    }
}

std::string App::InstallerStatusJson() const {
    /* The state machine the page renders: nothing that happens on the console is invented here, and
       the page keeps no state of its own (design section 14, item 29). */
    const bool transferring = m_page == Page::GameInstall && m_game_install != nullptr &&
                              m_install_screen == InstallScreen::Progress;
    /* A finished state describes the run that has *ended* -- and only while nothing new is being
       read.  The player who picks the next file without walking the console off the result screen
       starts a run while that screen is still up: reporting "failed" during it made the page stop
       posting parts on the spot, and the console then declared the browser gone after its thirty
       seconds of silence (measured in Chrome, 2026-10-16 -- both screens blaming the other). */
    const bool reading_a_new_run = m_upload_source != nullptr || m_http_pending_upload;
    const bool finished = !reading_a_new_run && m_page == Page::GameInstall &&
                          m_install_screen == InstallScreen::Result;
    /* The confirm screen is a state of its own: the package is read and described, nothing moves
       until someone presses start, and the page has to be able to mirror that card (and offer the
       same start) instead of showing "0%"/"100%" with no explanation (2026-10-19). */
    const bool confirming = m_page == Page::GameInstall && m_game_install != nullptr &&
                            m_install_screen == InstallScreen::Confirm;
    const char *state = "idle";
    if (transferring) {
        state = "downloading";
    } else if (confirming) {
        state = "confirm";
    } else if (finished) {
        switch (m_install_status.stage) {
            case install::InstallStage::Done: state = "done"; break;
            case install::InstallStage::Skipped: state = "skipped"; break;
            default: state = "failed"; break;
        }
    }
    /* The numbers the page draws.  A finished run reports itself as complete: the engine's counters
       describe what is *left* to transfer, so after the last byte they read 0 -- and a page that
       showed "100% of 0" as "0%" is exactly what the first hardware look caught. */
    const std::uint64_t package_bytes =
        m_game_install != nullptr ? install::PackageBytes(m_game_install->Task()) : 0;
    std::uint64_t received = m_install_status.done_bytes;
    std::uint64_t total = m_install_status.total_bytes;
    if (confirming) {
        /* Nothing has moved yet: the card is about the package itself, so its size is the package's
           -- the same number the console's confirm screen prints (design section 8). */
        received = 0;
        total = package_bytes;
    }
    if (finished) {
        const bool complete = m_install_status.stage == install::InstallStage::Done ||
                              m_install_status.stage == install::InstallStage::Skipped;
        received = complete ? package_bytes : received;
        total = complete ? package_bytes : total;
    }
    char buffer[64];
    std::string json = "{\"schema\":1,\"state\":\"";
    json += state;
    /* The page's labels are injected at load time, so an already-open tab keeps the old language
       until it reloads.  Naming the console's current language here lets the page notice the
       switch and reload itself (2026-10-19). */
    json += "\",\"lang\":\"";
    json += i18n::Current() == i18n::Language::English ? "en" : "zh-Hans";
    json += "\",\"source\":\"";
    json += m_upload_source != nullptr
                ? "upload"
                : (m_http_source != nullptr
                       ? "download"
                       : (m_game_install != nullptr ? "file" : "none"));
    json += "\",\"busy\":";
    json += transferring ? "true" : "false";
    /* An upload source exists from the moment `/upload/begin` opens the package, which is before
       the console reaches its Progress screen.  The page needs that window too: without it a
       reopened tab saw "idle", offered the picker, and every choice bounced off a 409 until the
       old source timed out (2026-10-21). */
    json += ",\"upload_active\":";
    json += (m_upload_source != nullptr || m_http_pending_upload) ? "true" : "false";
    /* How long the console will still wait for the *original* page's next part.  A page that
       replaced it (or a phone that lost the tab) cannot feed those bytes, so it shows the same
       countdown the console's footer does instead of a generic "busy". */
    {
        int wait_seconds = 0;
        const bool waiting_for_page =
            m_upload_source != nullptr && m_upload_source->PageWaitSeconds(&wait_seconds);
        json += ",\"wait_seconds\":";
        std::snprintf(buffer, sizeof(buffer), "%d", waiting_for_page ? wait_seconds : 0);
        json += buffer;
    }
    json += ",\"name\":\"";
    /* The console's own name for the run in hand -- the same line its cards print (design section
       14, item 29: the page invents nothing).  The installer's `detail` is a diagnostic ("meta=…,
       contents=…"): it belongs in log.txt, and it reached the page as an item name while every
       test stayed on the console side (2026-10-16). */
    json += net::EscapeForJs(m_game_install != nullptr ? PackageSummary() : std::string());
    json += "\",\"received\":";
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(received));
    json += buffer;
    json += ",\"total\":";
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(total));
    json += buffer;
    json += ",\"received_text\":\"";
    json += util::FormatSize(received);
    json += "\",\"total_text\":\"";
    json += util::FormatSize(total);
    json += "\",\"eta_text\":\"";
    /* The same sentences the console's card prints, worded once (InstallEtaText/InstallRateText):
       the page shows them next to the same byte counts, so the two screens agree to the digit. */
    json += net::EscapeForJs(transferring ? InstallEtaText() : std::string());
    json += "\",\"rate_text\":\"";
    json += net::EscapeForJs(transferring ? InstallRateText() : std::string());
    json += "\",\"content_index\":";
    /* The content being written, not the number finished -- the console's own counter adds one
       (and clamps at the total), and the page has to say "2 / 3" at the same moment it does
       (2026-10-19).  At the end done == total, so the clamp keeps it at "3 / 3". */
    const int current_content = install::CurrentContentNumber(m_install_status.done_contents,
                                                              m_install_status.total_contents);
    std::snprintf(buffer, sizeof(buffer), "%d", current_content);
    json += buffer;
    json += ",\"content_total\":";
    std::snprintf(buffer, sizeof(buffer), "%d", m_install_status.total_contents);
    json += buffer;
    json += ",\"content_name\":\"";
    json += net::EscapeForJs(m_install_status.current_name);
    /* What the console's own card says, so the browser can say it too: the one-line summary of the
       package, the "written" line of a finished run, and whether there is a cover to fetch. */
    json += "\",\"summary\":\"";
    json += net::EscapeForJs(m_game_install != nullptr ? PackageSummary() : std::string());
    json += "\",\"written\":\"";
    if (finished && m_install_status.stage == install::InstallStage::Done) {
        /* The page's own layout for this line: contents, size, and where it went. */
        char written[160];
        std::snprintf(written, sizeof(written), Tr(i18n::StringId::PageWrittenLine),
                      m_install_status.total_contents,
                      util::FormatSize(m_install_status.total_bytes).c_str(),
                      Tr(m_install_target == install::TargetStorage::Nand
                             ? i18n::StringId::StorageSystem
                             : i18n::StringId::StorageSdCard));
        json += net::EscapeForJs(written);
    }
    json += "\",\"hasCover\":";
    json += (m_game_install != nullptr && !m_game_install->IconJpeg().empty()) ? "true" : "false";
    /* Which build is serving this page.  `Cache-Control: no-store` keeps a browser from *keeping* an
       old page, but nothing makes an already-open tab swap the JavaScript it is running for a newer
       one -- only a reload does.  So the page compares this with its own stamp and reloads itself
       (when nothing is running), which is what "the phone picked up the new code" has to mean when
       nobody is going to press Ctrl-Shift-R on a phone (2026-10-17). */
    json += ",\"build\":\"";
    json += kBuildStamp;
    json += "\"";
    /* The upload task's id: a page that reloaded (or a second tab that wants to take over) reads it
       from here and keeps feeding the same task, which is why the id is in the status payload and
       not only in the answer to `POST /upload/begin` (design section 29).  It outlives the source on
       purpose: a page has to be able to tell "this finished state is the run *I* fed" from "that is
       the previous run's result still on the console's screen" -- without it, a page that picks the
       next file stops posting parts the moment it reads the old verdict (2026-10-16). */
    json += ",\"taskId\":\"";
    json += m_http_upload_task_id;
    /* The run in hand, and where it would write: what the page needs to mirror the confirm card and
       to ask for a start or a storage change that the console can check (2026-10-19). */
    json += "\",\"runId\":";
    std::snprintf(buffer, sizeof(buffer), "%d", m_game_install != nullptr ? m_run_id : 0);
    json += buffer;
    json += ",\"target\":\"";
    json += m_install_target == install::TargetStorage::Nand ? "nand" : "sd";
    json += "\",\"target_text\":\"";
    json += net::EscapeForJs(Tr(m_install_target == install::TargetStorage::Nand
                                    ? i18n::StringId::StorageSystem
                                    : i18n::StringId::StorageSdCard));
    json += "\"";
    /* The two storage blocks, exactly as the console draws them: text, whether they can be read, how
       full they are, and which one is selected.  A storage that cannot be read says "unavailable" in
       its text and carries ok=false -- the page disables that block instead of offering it. */
    json += ",\"sd\":{\"text\":\"";
    json += net::EscapeForJs(m_status_storage_text[0]);
    json += "\",\"ok\":";
    json += m_status_storage_ok[0] ? "true" : "false";
    json += ",\"used\":";
    std::snprintf(buffer, sizeof(buffer), "%.3f", m_status_storage_used[0]);
    json += buffer;
    json += ",\"selected\":";
    json += m_install_target == install::TargetStorage::SdCard ? "true" : "false";
    json += "},\"nand\":{\"text\":\"";
    json += net::EscapeForJs(m_status_storage_text[1]);
    json += "\",\"ok\":";
    json += m_status_storage_ok[1] ? "true" : "false";
    json += ",\"used\":";
    std::snprintf(buffer, sizeof(buffer), "%.3f", m_status_storage_used[1]);
    json += buffer;
    json += ",\"selected\":";
    json += m_install_target == install::TargetStorage::Nand ? "true" : "false";
    json += "}";
    /* The confirm card's two extra lines, worded here so both devices say the same thing: whether the
       agent can follow this package, and how much an interrupted run already has. */
    bool agent_note_warn = false;
    const std::string agent_note = PackageAgentNote(&agent_note_warn);
    json += ",\"agent_note\":\"";
    json += net::EscapeForJs(agent_note);
    json += "\",\"agent_note_warn\":";
    json += agent_note_warn ? "true" : "false";
    json += ",\"resume_text\":\"";
    if (confirming && m_resumed_bytes > 0) {
        json += net::EscapeForJs(i18n::Format(i18n::StringId::ConfirmResumeNote,
                                              util::FormatSize(m_resumed_bytes).c_str()));
    }
    json += "\"";
    /* The same "what now" the console's result card gives: the agent can go in when this run left a
       version it supports, and only when it is not already installed. */
    json += ",\"agent_next\":";
    const bool agent_next =
        finished && m_game_install != nullptr && !AgentInstalled() &&
        AgentSupportedFor(m_game_install->Package().cnmt.title_id,
                          m_install_status.stage == install::InstallStage::Skipped
                              ? m_install_status.installed_version
                              : m_install_status.effective_version);
    json += agent_next ? "true" : "false";
    /* A source-screen error belongs on the browser too.  The upload path tears down its source
       before returning there, so without this field the page saw only "pick the file again" and
       lost the actionable reason the console was showing. */
    json += ",\"error\":\"";
    json += net::EscapeForJs(!finished ? InstallErrorText() : std::string());
    json += "\"";
    json += ",\"message\":\"";
    json += net::EscapeForJs(finished ? InstallOutcomeLine() : std::string());
    json += "\",\"pending\":";
    if (m_have_pending_task) {
        json += "{\"name\":\"";
        json += net::EscapeForJs(m_pending_task.name);
        json += "\",\"received\":\"";
        json += util::FormatSize(m_pending_live_bytes);
        json += "\",\"total\":\"";
        json += util::FormatSize(install::TotalBytes(m_pending_task));
        json += "\"}";
    } else {
        json += "null";
    }
    json += "}";
    /* One sanity check on the assembled payload.  This JSON is hand-built, and a missing quote is
       not something the compiler can see: `"build":"…,"taskId"` shipped like that on 2026-10-17 and
       the page could only report "the console is offline".  Parsing our own output costs
       microseconds per status request and turns that whole class into one log line. */
    {
        json::Value parsed;
        std::string parse_error;
        if (!json::Parse(json, &parsed, &parse_error) && m_log != nullptr) {
            m_log->Line("[t=%llums] http: the status payload is not valid JSON (%s): %s",
                        static_cast<unsigned long long>(
                            armTicksToNs(armGetSystemTick()) / 1000000ull),
                        parse_error.c_str(), json.c_str());
        }
    }
    return json;
}

net::HttpResponse App::ServeHttp(const net::HttpRequest &request) {
    net::HttpResponse response;
    if (request.method == "GET" && (request.path == "/" || request.path == "/index.html")) {
        response.content_type = "text/html; charset=utf-8";
        const std::string_view embedded = payload::EmbeddedInstallerPage();
        if (embedded.empty()) {
            response.status = 501;
            response.content_type = "text/plain; charset=utf-8";
            response.body = "this build carries no installer page\n";
            return response;
        }
        std::string page(embedded);
        /* The page's wording, its language and the address it was reached at all come from here:
           the console's own i18n table and its own address, so the two screens agree (design
           section 14, items 15 and 27). */
        page = net::SubstituteLabels(page, net::kPageLabelsMarker, net::BuildPageLabelsJson());
        page = net::SubstituteLabels(page, net::kPageLangMarker,
                                     i18n::Current() == i18n::Language::English ? "en" : "zh-Hans");
        page = net::SubstituteLabels(page, net::kPageAddressMarker, InstallerAddressText());
        page = net::SubstituteLabels(page, net::kPageVersionMarker,
                                     net::EscapeForJs(kBuildStamp));
        page = net::SubstituteLabels(page, net::kPageVersionTextMarker,
                                     net::EscapeForJs(m_footer_text));
        response.body = std::move(page);
        return response;
    }
    if (request.method == "GET" && request.path == "/status.json") {
        response.content_type = "application/json; charset=utf-8";
        response.body = InstallerStatusJson();
        return response;
    }
    if (request.method == "GET" && request.path == "/cover.jpg") {
        /* The package's own cover, exactly as `ns` handed it over: the browser gets the JPEG the
           engine already holds in memory (the design's page shows the same picture the console's
           cards do, and it is the package's, not the installed game's -- a package for a game that
           is not installed still has one). */
        if (m_game_install == nullptr || m_game_install->IconJpeg().empty()) {
            response.status = 404;
            response.body = "no cover\n";
            return response;
        }
        response.content_type = "image/jpeg";
        const std::vector<std::uint8_t> &jpeg = m_game_install->IconJpeg();
        response.body.assign(jpeg.begin(), jpeg.end());
        return response;
    }
    if (request.method == "POST" && request.path == "/url") {
        const std::string address = request.body;
        if (address.empty() || address.size() > 2048) {
            response.status = 400;
            response.body = "empty address\n";
            return response;
        }
        /* Not opened here: this runs inside the socket pass, and opening a package touches ncm, the
           engine and the screens.  The UI loop picks it up on the next frame. */
        m_http_pending_url = address;
        response.body = "ok\n";
        return response;
    }
    /* ---- the two things the page does that change what the console is doing ---- */
    if (request.method == "POST" && request.path == "/start") {
        /* The page's Ⓐ on the confirm card.  Same gate as the console's own key: a package in hand,
           the confirm screen up, and the run the page is looking at (`runId`) -- so a stale tab can
           neither start a run it was not told about nor start the same one twice. */
        json::Value root;
        std::string parse_error;
        if (!json::Parse(request.body, &root, &parse_error) || !root.IsObject()) {
            response.status = 400;
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"code\":\"bad-request\"}\n";
            return response;
        }
        const json::Value *run_value = root.Find("runId");
        const int run_id =
            run_value != nullptr ? static_cast<int>(json::IntOr(*run_value, 0)) : 0;
        if (m_page != Page::GameInstall || m_install_screen != InstallScreen::Confirm ||
            m_game_install == nullptr || run_id != m_run_id) {
            response.status = 409;
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"code\":\"not-confirming\"}\n";
            return response;
        }
        /* Not started here: this runs inside the socket pass, and the transfer owns the UI thread for
           minutes.  The UI loop picks it up on the next frame, exactly like the console's `A`. */
        m_http_pending_start = true;
        response.content_type = "application/json; charset=utf-8";
        response.body = "{\"ok\":true}\n";
        return response;
    }
    if (request.method == "POST" && request.path == "/back") {
        /* The page's Ⓑ on the confirm and result cards.  Only those screens may step back, and only
           for the run the page was looking at -- a stale tab cannot close somebody else's result. */
        json::Value root;
        std::string parse_error;
        if (!json::Parse(request.body, &root, &parse_error) || !root.IsObject()) {
            response.status = 400;
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"code\":\"bad-request\"}\n";
            return response;
        }
        const json::Value *run_value = root.Find("runId");
        const int run_id =
            run_value != nullptr ? static_cast<int>(json::IntOr(*run_value, 0)) : 0;
        const int expected = m_game_install != nullptr ? m_run_id : 0;
        if (m_page != Page::GameInstall ||
            (m_install_screen != InstallScreen::Result &&
             m_install_screen != InstallScreen::Confirm) ||
            run_id != expected) {
            response.status = 409;
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"code\":\"not-backable\"}\n";
            return response;
        }
        m_http_pending_back = true;
        response.content_type = "application/json; charset=utf-8";
        response.body = "{\"ok\":true}\n";
        return response;
    }
    if (request.method == "POST" && request.path == "/target") {
        /* The page's storage blocks.  It goes through the same selector the console's header uses,
           which means the package is re-opened on the new storage -- the page's own note says so. */
        json::Value root;
        std::string parse_error;
        if (!json::Parse(request.body, &root, &parse_error) || !root.IsObject()) {
            response.status = 400;
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"code\":\"bad-request\"}\n";
            return response;
        }
        const json::Value *run_value = root.Find("runId");
        const json::Value *target_value = root.Find("target");
        const int run_id =
            run_value != nullptr ? static_cast<int>(json::IntOr(*run_value, 0)) : 0;
        const std::string target =
            target_value != nullptr ? target_value->StringOr(std::string()) : std::string();
        if (target != "sd" && target != "nand") {
            response.status = 400;
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"code\":\"bad-target\"}\n";
            return response;
        }
        if (m_page != Page::GameInstall || !InstallTargetSelectable() ||
            (m_game_install != nullptr && run_id != m_run_id)) {
            response.status = 409;
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"code\":\"not-selectable\"}\n";
            return response;
        }
        m_http_pending_target = true;
        m_http_target = target == "nand" ? install::TargetStorage::Nand
                                         : install::TargetStorage::SdCard;
        response.content_type = "application/json; charset=utf-8";
        response.body = "{\"ok\":true}\n";
        return response;
    }
    /* ---- the upload path: the browser is the source, one posted range at a time ---- */
    if (request.method == "POST" && request.path == "/upload/begin") {
        /* {name, size}: the file the page picked.  One upload at a time, and never while a transfer
           is running -- a task has exactly one source (design section 3). */
        if (m_upload_source != nullptr || m_http_pending_upload ||
            (m_page == Page::GameInstall && m_install_screen == InstallScreen::Progress)) {
            response.status = 409;
            response.body = "{\"code\":\"busy\"}\n";
            return response;
        }
        json::Value root;
        std::string parse_error;
        if (!json::Parse(request.body, &root, &parse_error) || !root.IsObject()) {
            response.status = 400;
            response.body = "{\"code\":\"bad-request\"}\n";
            return response;
        }
        const json::Value *name = root.Find("name");
        const json::Value *size = root.Find("size");
        if (name == nullptr || size == nullptr) {
            response.status = 400;
            response.body = "{\"code\":\"bad-request\"}\n";
            return response;
        }
        m_http_upload_name = name->StringOr(std::string());
        m_http_upload_size = static_cast<std::uint64_t>(json::IntOr(*size, 0));
        if (m_http_upload_name.empty() || m_http_upload_size == 0) {
            response.status = 400;
            response.body = "{\"code\":\"bad-request\"}\n";
            return response;
        }
        /* The id the page quotes with every part: it makes "this is the tab feeding the task I
           asked for" checkable, and a stale tab's parts are refused with a 409 (design section 8). */
        char id[32];
        std::snprintf(id, sizeof(id), "%08x%04x", static_cast<unsigned>(armGetSystemTick() & 0xFFFFFFFFu),
                      static_cast<unsigned>(m_http_upload_serial++ & 0xFFFF));
        m_http_upload_task_id = id;
        m_http_pending_upload = true;
        response.content_type = "application/json; charset=utf-8";
        response.body = "{\"taskId\":\"" + m_http_upload_task_id + "\"}\n";
        return response;
    }
    if (request.method == "GET" && request.path == "/upload/next") {
        if (m_upload_source == nullptr) {
            /* Between `POST /upload/begin` and the next frame the source does not exist yet: that is
               a moment, not a refusal.  Saying "wait" keeps every client from having to know about
               the console's frame loop. */
            response.content_type = "application/json; charset=utf-8";
            response.body = m_http_pending_upload ? "{\"wait\":true}\n"
                                                  : "{\"code\":\"no-task\"}\n";
            if (!m_http_pending_upload) {
                response.status = 409;
            }
            return response;
        }
        if (net::QueryValue(request.query, "taskId") != m_http_upload_task_id) {
            response.status = 409;
            response.body = "{\"code\":\"stale-task\"}\n";
            return response;
        }
        std::uint64_t offset = 0;
        std::size_t size = 0;
        char numbers[96];
        if (!m_upload_source->Want(&offset, &size)) {
            /* Nothing is being read this instant: the package is on the confirm screen, or the run
               is over.  `wait` is not an error -- the page simply asks again. */
            response.content_type = "application/json; charset=utf-8";
            response.body = "{\"wait\":true}\n";
            return response;
        }
        std::snprintf(numbers, sizeof(numbers), "{\"offset\":%llu,\"size\":%zu}\n",
                      static_cast<unsigned long long>(offset), size);
        response.content_type = "application/json; charset=utf-8";
        response.body = numbers;
        return response;
    }
    if (request.method == "POST" && request.path == "/upload/part") {
        if (m_upload_source == nullptr) {
            response.status = 409;
            response.body = "{\"code\":\"no-task\"}\n";
            return response;
        }
        if (net::QueryValue(request.query, "taskId") != m_http_upload_task_id) {
            response.status = 409;
            response.body = "{\"code\":\"stale-task\"}\n";
            return response;
        }
        std::uint64_t want_offset = 0;
        std::size_t want_size = 0;
        const std::uint64_t offset =
            std::strtoull(net::QueryValue(request.query, "offset").c_str(), nullptr, 10);
        if (!m_upload_source->Want(&want_offset, &want_size) ||
            !install::UploadPartMatches(want_offset, want_size, offset, request.body_length)) {
            /* Out of step (a dropped part, a stale page, a mirrored tab): tell it what is expected
               instead of writing bytes into the wrong place -- the design's 409 (section 8). */
            response.status = 409;
            char message[128];
            std::snprintf(message, sizeof(message),
                          "{\"code\":\"wrong-offset\",\"expected\":%llu,\"size\":%zu}\n",
                          static_cast<unsigned long long>(want_offset), want_size);
            response.content_type = "application/json; charset=utf-8";
            response.body = message;
            return response;
        }
        /* The body goes straight to the installer: the fetch thread is waiting for exactly these
           bytes, and buffering megabytes here would be work for nobody (see HttpUploadSource). */
        response.handoff = true;
        response.handoff_offset = offset;
        response.handoff_length = request.body_length;
        return response;
    }
    response.status = 404;
    response.body = "not found\n";
    return response;
}

/* The install an interruption left behind, as its own screen.  It is the first thing the install
   page shows while a task file exists, because the space its placeholders hold is otherwise
   invisible: "continue" finishes what was started, "give up" hands the space back, and "pick
   something else" leaves the task alone for later (the details page keeps listing it). */
void App::RenderInstallPending(Surface surface) {
    m_actions.clear();
    const int card_x = kMargin;
    const int card_top = kHeaderHeight + 24;
    const int card_width = surface.width - kMargin * 2;
    const int card_height = 214;
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);

    const std::uint64_t total = install::TotalBytes(m_pending_task);
    m_font.DrawBold(surface, card_x + 36, card_top + 22, kFontHeading, kWarn,
                    Tr(i18n::StringId::PendingTitle));
    /* The container's own name is what says *which* install this is: the package's title is not in
       the task file, and a name the player recognises beats a content id. */
    m_font.Draw(surface, card_x + 36, card_top + 68, kFontBody, kText,
                m_font.Fit(m_pending_task.name, kFontBody, card_width - 72, 1).c_str());
    const std::string progress =
        i18n::Format(i18n::StringId::PendingProgress, util::FormatSize(m_pending_live_bytes).c_str(),
                     util::FormatSize(total).c_str());
    m_font.Draw(surface, card_x + 36, card_top + 106, kFontBody, kText, progress.c_str());
    const std::string reserved_text =
        i18n::Format(i18n::StringId::PendingReserved,
                     util::FormatSize(m_pending_live_reserved).c_str());
    m_font.Draw(surface, card_x + 36, card_top + 140, kFontSmall, kSubtle, reserved_text.c_str());
    if (m_pending_restarts) {
        /* The console has nothing left of it: saying that now is kinder than letting the player press
           "continue" and watch the same gigabytes download twice. */
        m_font.Draw(surface, card_x + 36, card_top + 172, kFontSmall, kWarn,
                    m_font.Fit(Tr(i18n::StringId::PendingGone), kFontSmall, card_width - 72, 1)
                        .c_str());
    } else if (m_pending_task.source == install::TaskSource::HttpUpload) {
        /* "Continue" for an upload happens in the browser, not on this screen, so the card says
           where that is before the player presses anything. */
        m_font.Draw(surface, card_x + 36, card_top + 172, kFontSmall, kSubtle,
                    m_font.Fit(Tr(i18n::StringId::PendingUploadResume), kFontSmall, card_width - 72, 1)
                        .c_str());
    } else if (!m_install_error.empty()) {
        m_font.Draw(surface, card_x + 36, card_top + 172, kFontSmall, kWarn,
                    m_font.Fit(m_install_error, kFontSmall, card_width - 72, 1).c_str());
    }

    /* Three entries, the same card shape as the source screen: they are the only ways forward from
       here, and each one says what it does to the task. */
    struct Entry {
        const char *key;
        i18n::StringId label;
        int action;
    };
    const Entry entries[3] = {{"A", i18n::StringId::PendingResume, kActionPendingResume},
                              {"X", i18n::StringId::PendingAbandon, kActionPendingAbandon},
                              {"Y", i18n::StringId::PendingPickNew, kActionPendingPickNew}};
    const int row_top = card_top + card_height + 26;
    constexpr int kRowHeight = 92;
    constexpr int kRowGap = 16;
    for (int i = 0; i < 3; ++i) {
        const Rect rect{card_x, row_top + i * (kRowHeight + kRowGap), card_width, kRowHeight};
        FillRoundedRect(surface, rect.x, rect.y, rect.w, rect.h, 16, kBorder);
        FillRoundedRect(surface, rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, 13,
                        m_focus == entries[i].action ? kCardDisabled : kCard);
        DrawKeyBadge(surface, m_font, rect.x + 46, rect.y + rect.h / 2, 20, entries[i].key,
                     kHeader, kOnHeader);
        m_font.Draw(surface, rect.x + 92,
                    rect.y + (rect.h - m_font.LineHeight(kFontHeading)) / 2, kFontHeading,
                    i == 1 ? kWarn : kText, Tr(entries[i].label));
        m_actions.push_back({entries[i].action, rect, true});
    }
    m_focus = m_actions.empty() ? -1 : 0;
}

/* The installer's own page.  Which of its screens shows is one variable, so the dispatcher is a
   switch and each screen draws itself; nothing else in the app is allowed to draw while this page
   is up (see AddChromeActions), which is what makes "install is exclusive" true on screen too. */
void App::RenderGameInstall(Surface surface) {
    if (m_install_screen == InstallScreen::Pending) {
        RenderInstallPending(surface);
        return;
    }
    if (m_install_screen == InstallScreen::Files) {
        RenderInstallFiles(surface);
        return;
    }
    if (m_install_screen == InstallScreen::Confirm) {
        RenderInstallConfirm(surface);
        return;
    }
    if (m_install_screen == InstallScreen::Url) {
        RenderInstallUrl(surface);
        return;
    }
    if (m_install_screen == InstallScreen::Upload) {
        RenderInstallUpload(surface);
        return;
    }
    switch (m_install_screen) {
        case InstallScreen::Progress: RenderInstallProgress(surface); return;
        case InstallScreen::Result: RenderInstallResult(surface); return;
        case InstallScreen::Pending: /* handled above, before the switch */ break;
        case InstallScreen::Choose:
        case InstallScreen::Files:
        case InstallScreen::Url:
        case InstallScreen::Upload:
        case InstallScreen::Confirm: break;
    }

    /* Pick a source.  This screen shows nothing but the three entries and what formats are
       accepted: everything after it follows from that choice. */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    m_font.Draw(surface, kMargin, kHeaderHeight + 24, kFontSmall, kSubtle,
                Tr(i18n::StringId::InstallFormats));

    struct Entry {
        const char *key;
        i18n::StringId label;
        i18n::StringId sub;
        int action;
    };
    const Entry entries[3] = {
        {"Y", i18n::StringId::InstallPickFile, i18n::StringId::InstallPickFileSub,
         kActionInstallLocal},
        {"X", i18n::StringId::InstallDownload, i18n::StringId::InstallDownloadSub,
         kActionInstallDownload},
        {"A", i18n::StringId::InstallUpload, i18n::StringId::InstallUploadSub,
         kActionInstallUpload}};
    constexpr int kCardGap = 24;
    constexpr int kCardHeight = 210;
    const int card_top = kHeaderHeight + 70;
    const int card_width = (width - kCardGap * 2) / 3;
    for (int i = 0; i < 3; ++i) {
        const int x = kMargin + i * (card_width + kCardGap);
        const Rect rect{x, card_top, card_width, kCardHeight};
        FillRoundedRect(surface, rect.x, rect.y, rect.w, rect.h, 18, kBorder);
        FillRoundedRect(surface, rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, 15,
                        m_focus == entries[i].action ? kCardDisabled : kCard);
        DrawKeyBadge(surface, m_font, rect.x + 46, rect.y + rect.h / 2, 22, entries[i].key, kHeader,
                     kOnHeader);
        /* The labels live to the right of the badge and are fitted to what is left of the card.
           Centring them on the card (which ignored the badge) worked only while the labels were
           the short Chinese ones: in English "Download and install" ran under the Ⓧ and out of
           both card edges.  The sub-label gets two lines to wrap onto, the title one. */
        const int text_x = rect.x + 46 + 22 + 18;
        const int text_width = rect.x + rect.w - 24 - text_x;
        const std::string label =
            m_font.Fit(Tr(entries[i].label), kFontHeading + 8, text_width, 2);
        const std::string sub = m_font.Fit(Tr(entries[i].sub), kFontSmall, text_width, 2);
        const int label_height = m_font.LineCount(label, kFontHeading + 8, text_width) *
                                 m_font.LineHeight(kFontHeading + 8);
        const int sub_height =
            m_font.LineCount(sub, kFontSmall, text_width) * m_font.LineHeight(kFontSmall);
        const int gap = 10;
        const int block_top = rect.y + (rect.h - (label_height + gap + sub_height)) / 2;
        m_font.Draw(surface, text_x, block_top, kFontHeading + 8, kText, label.c_str(), text_width);
        m_font.Draw(surface, text_x, block_top + label_height + gap, kFontSmall, kSubtle,
                    sub.c_str(), text_width);
        m_actions.push_back({entries[i].action, rect, true});
    }
    m_focus = m_actions.empty() ? -1 : 0;
}

/* The result screen: four titles, and three rows that say what it was, what happened and what
   the player can do now.  Nothing here points at another page -- the design keeps the whole
   answer on this screen (section 8). */

/* The file picker: one card, the directory as rows, the focus as a highlighted row with the mark
   on its *lower* edge.  Every row's hit area comes from the same kRowHeight the list uses. */
void App::RenderInstallFiles(Surface surface) {
    m_actions.clear();
    const int card_x = kMargin;
    const int card_top = kHeaderHeight + 24;
    const int card_width = surface.width - kMargin * 2;
    const int card_height = surface.height - card_top - kFooterHeight - 24;
    /* The list leaves room for a one-line reason at the bottom: the close-the-game hint lives
       there, and a clamped row would draw it half off the card. */
    m_file_list.SetBox(card_width, card_height - 60 - 34);
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);

    m_font.Draw(surface, card_x + 24, card_top + 14, kFontHeading, kText,
                Tr(i18n::StringId::InstallPickFile));
    m_font.Draw(surface, card_x + 24 + m_font.Measure(Tr(i18n::StringId::InstallPickFile),
                                                      kFontHeading) + 18,
                card_top + 24, kFontSmall, kSubtle, m_file_dir.c_str());

    if (!m_install_error.empty()) {
        m_font.Draw(surface, card_x + 24, card_top + card_height - 46, kFontSmall, kBad,
                    m_install_error == install::kErrorGameRunning
                        ? Tr(i18n::StringId::InstallGameRunning)
                        : m_install_error.c_str());
    }
    const ui::ListWindow window = m_file_list.Window();
    const int list_top = card_top + 60;
    const std::vector<ui::ListItem> &items = m_file_list.Items();
    if (items.empty()) {
        m_font.Draw(surface, card_x + 24, list_top + 8, kFontBody, kSubtle,
                    Tr(i18n::StringId::InstallNoFiles));
        return;
    }
    for (int row = 0; row < window.count; ++row) {
        const int index = window.first + row;
        if (index >= static_cast<int>(items.size())) {
            break;
        }
        const ui::ListItem &item = items[static_cast<std::size_t>(index)];
        const int y = list_top + row * ui::ListView::kRowHeight;
        /* The text is centred in the highlight rather than pinned to its top: the row is 66 px
           (58 of them highlight) while a body line is only 29, so "text at the top" left 6 px of
           room above the name and 23 px below it -- the name looked stuck to the upper edge with
           a gap down to the mark.  The note on the right keeps the same offset from the name. */
        const int text_y = y - 6 + (ui::ListView::kRowHeight - 8 - m_font.LineHeight(kFontBody)) / 2;
        const Rect hit{card_x + 16, y - 6, card_width - 32, ui::ListView::kRowHeight - 8};
        if (index == m_file_list.Focus()) {
            FillRoundedRect(surface, hit.x, hit.y, hit.w, hit.h, 12, kCardDisabled);
            FillRoundedRect(surface, hit.x, hit.y + hit.h - 3, hit.w, 3, 0, kHeader);
        }
        const Color ink = item.selectable ? kText : kSubtle;
        const int note_width = m_font.Measure(item.note.c_str(), kFontSmall);
        /* Truncated to the room left of the note: a long package name painted over by the note
           (which now carries a size as well) would hide exactly the number the row is there to
           show.  The full name is in the log line when a file is picked. */
        const int name_x = card_x + 34;
        const int name_width = card_x + card_width - 34 - note_width - 24 - name_x;
        m_font.Draw(surface, name_x, text_y, kFontBody, ink,
                    m_font.Fit(item.name, kFontBody, name_width, 1).c_str());
        m_font.Draw(surface, card_x + card_width - 34 - note_width, text_y + 4, kFontSmall, ink,
                    item.note.c_str());
        if (item.selectable) {
            m_actions.push_back({kActionInstallPickRow, hit, true});
        }
    }
}

/* The download screen: one instruction, one control (the system keyboard), and the reason when an
   address was refused.  Nothing here invents an address -- the design is explicit that this app
   does not host or recommend one -- so the screen is the player's own address, typed or pasted. */
/* The address screen.  Its shape is the one the mockups fixed: a line of context, the way in as a
   whole clickable card, the rule the address has to satisfy as small print, and -- only when
   something went wrong -- a strip that names the reason and the address it belongs to.

   The second way in (open the console's page on a phone and paste the address there, which saves
   typing every character) needs the HTTP server, so it lands with it; this screen is laid out so
   that adding it below the card changes nothing else (design section 8). */
/* One card, both source screens, and the only thing they can say with no network: the keyboard and
   the page both live on the network, so neither source has a way in until settings say otherwise.
   The reason is deliberately absent -- "no network" is one of the few states the player fixes
   themselves, and the sentence is already the fix. */
static void DrawNoNetworkCard(Surface surface, Font &font, const char *title, const char *hint) {
    const int card_x = kMargin;
    const int card_w = surface.width - kMargin * 2;
    const int card_y = kHeaderHeight + 52;
    const int card_h = 148;
    FillRoundedRect(surface, card_x, card_y, card_w, card_h, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_y + 3, card_w - 6, card_h - 6, 15, kCard);
    constexpr int kTextGap = 12;
    const int hint_width = card_w - 120;
    const int hint_lines = font.LineCount(hint, kFontSmall, hint_width);
    const int hint_ink_h = (hint_lines - 1) * font.LineHeight(kFontSmall) + kFontSmall;
    const int text_h = kFontHeading + kTextGap + hint_ink_h;
    const int text_y = card_y + (card_h - text_h) / 2;
    DrawKeyBadge(surface, font, card_x + 46, card_y + card_h / 2, 22, "!", kWarn, kOnHeader);
    font.DrawBold(surface, card_x + 82, text_y, kFontHeading, kText, title);
    font.Draw(surface, card_x + 82, text_y + kFontHeading + kTextGap, kFontSmall, kSubtle, hint,
              hint_width);
}

void App::RenderInstallUrl(Surface surface) {
    m_actions.clear();
    const int card_x = kMargin;
    const int card_w = surface.width - kMargin * 2;

    m_font.Draw(surface, kMargin, kHeaderHeight + 22, kFontSmall, kSubtle,
                Tr(i18n::StringId::InstallDownloadSub));

    if (m_install_no_network) {
        /* No network: say what to do about it, and nothing else.  An address or a page nobody can
           reach would be worse than none at all, so neither is drawn. */
        DrawNoNetworkCard(surface, m_font, Tr(i18n::StringId::InstallNoNetworkTitle),
                          Tr(i18n::StringId::InstallNoNetworkHint));
        DrawInstallUrlNotes(surface, kHeaderHeight + 52 + 148 + 20, true);
        return;
    }

    const int card_y = kHeaderHeight + 112;
    const int card_h = 140;
    const Rect rect{card_x, card_y, card_w, card_h};
    FillRoundedRect(surface, rect.x, rect.y, rect.w, rect.h, 18, kBorder);
    FillRoundedRect(surface, rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, 15, kCard);
    DrawKeyBadge(surface, m_font, card_x + 46, card_y + card_h / 2, 22, "A", kHeader, kOnHeader);
    /* Centred, like the home page's main button: the badge on the left says "this is a key", and
       centred text is what the app's other full-width buttons do. */
    const int centre = surface.width / 2;
    constexpr int kTextGap = 12;
    const bool has_last = !m_download_url.empty();
    const int text_h = kFontHeading + kTextGap + kFontSmall +
                       (has_last ? kTextGap + kFontSmall : 0);
    const int text_y = card_y + (card_h - text_h) / 2;
    const auto centred = [&](const std::string &line, int size, Color color, int y) {
        m_font.Draw(surface, centre - m_font.Measure(line, size) / 2, y, size, color,
                    line.c_str());
    };
    centred(Tr(i18n::StringId::InstallTypeUrl), kFontHeading, kText, text_y);
    centred(Tr(i18n::StringId::InstallUrlHint), kFontSmall, kSubtle,
            text_y + kFontHeading + kTextGap);
    if (has_last) {
        /* The address that was tried: retrying is then a one-character job instead of typing the
           whole thing again. */
        const std::string last = m_font.Fit(
            i18n::Format(i18n::StringId::InstallUrlLast, m_download_url.c_str()), kFontSmall,
            card_w - 200, 1);
        centred(last, kFontSmall, kSubtle,
                text_y + kFontHeading + kTextGap + kFontSmall + kTextGap);
    }
    m_actions.push_back({kActionInstallUrl, rect, true});

    /* The failure strip: the reason, and the address it belongs to. */
    int y = card_y + card_h;
    if (!m_install_error.empty()) {
        /* The reason is player-facing wording and can wrap (English is longer than Chinese), so the
           strip is measured from what it has to hold rather than fixed: a clamped line would draw
           outside its own border. */
        const std::string reason = InstallErrorText();
        const int reason_lines = m_font.LineCount(reason, kFontBody, card_w - 48);
        const int address_lines = m_download_url.empty()
                                      ? 0
                                      : m_font.LineCount(m_download_url, kFontSmall, card_w - 48);
        const int strip_h = 12 + reason_lines * m_font.LineHeight(kFontBody) + 4 +
                            address_lines * m_font.LineHeight(kFontSmall) + 12;
        y += 20;
        /* Center the visible ink block, not its font line boxes: CJK has no descender pixels, so a
           line-height box centered inside the strip still reads several pixels high. */
        const int reason_ink_h =
            (reason_lines - 1) * m_font.LineHeight(kFontBody) + kFontBody;
        const int address_ink_h = address_lines == 0
                                      ? 0
                                      : (address_lines - 1) * m_font.LineHeight(kFontSmall) +
                                            kFontSmall;
        const int group_gap = address_lines == 0 ? 0 : 4;
        const int group_h = reason_ink_h + group_gap + address_ink_h;
        const int group_y = y + (strip_h - group_h) / 2;
        FillRoundedRect(surface, card_x, y, card_w, strip_h, 12, kWarnTint);
        StrokeRect(surface, card_x, y, card_w, strip_h, 2, kWarnBright);
        m_font.DrawBold(surface, card_x + 24, group_y, kFontBody, kWarn, reason.c_str(),
                        card_w - 48);
        if (!m_download_url.empty()) {
            m_font.Draw(surface, card_x + 24, group_y + reason_ink_h + group_gap, kFontSmall,
                        kSubtle, m_font.Fit(m_download_url, kFontSmall, card_w - 48, 1).c_str());
        }
        y += strip_h;
        /* The rule the address had to satisfy is what the strip just said; only the format line
           is worth the room here. */
        DrawInstallUrlNotes(surface, y + 24, false);
        return;
    }
    /* The second way in, and the one the design calls the main one for this source: the phone or
       the computer opens the address below and pastes the download address into the page, so
       nothing has to be typed letter by letter on a console. */
    y = RenderInstallerAddressBlock(surface, y + 20, false);
    DrawInstallUrlNotes(surface, y + 22, true);
}

/* The upload source's screen: nothing above the block, because there is no console-side action at
   all -- the page is the way in.  The design keeps the two source screens on one layout so that a
   player who has seen one recognises the other at a glance (section 8). */
void App::RenderInstallUpload(Surface surface) {
    m_actions.clear();
    m_font.Draw(surface, kMargin, kHeaderHeight + 22, kFontSmall, kSubtle,
                Tr(i18n::StringId::InstallUploadSub));
    if (m_install_no_network) {
        DrawNoNetworkCard(surface, m_font, Tr(i18n::StringId::InstallNoNetworkTitle),
                          Tr(i18n::StringId::InstallNoNetworkHint));
        /* No Range rule here: an upload posts its bytes in chunks, it never range-requests. */
        DrawInstallUrlNotes(surface, kHeaderHeight + 52 + 148 + 20, false);
        return;
    }
    const int y = RenderInstallerAddressBlock(surface, kHeaderHeight + 52, true);
    DrawInstallUrlNotes(surface, y + 22, false);
}

/* The address of the page the console is serving right now, as a code.  Re-encoded only when the
   address changed: this is drawn every frame while its screen is up, and the address changes when
   the server starts (which is what turns it from empty into a real one). */
const util::QrCode &App::InstallerQrCode() {
    const std::string address = InstallerAddressText();
    if (address != m_address_qr_text) {
        m_address_qr_text = address;
        m_address_qr = address.empty() ? util::QrCode{} : util::EncodeQr(address);
        if (!address.empty() && m_address_qr.Empty()) {
            Trace("http: the address is too long to encode as a code (%zu chars)", address.size());
        }
    }
    return m_address_qr;
}

/* The block the two source screens share: what to open, the address itself, what the page is for,
   and the state the console is in while it waits.  `for_upload` picks the one line that differs --
   the page is the same page and offers both ("paste an address", "pick a file"), so the console
   says which of the two this screen is waiting for. */
int App::RenderInstallerAddressBlock(Surface surface, int y, bool for_upload) {
    const int card_x = kMargin;
    const int card_w = surface.width - kMargin * 2;
    const int card_h = 216;
    const int plate_side = 170;
    const int plate_x = card_x + card_w - 24 - plate_side;
    const int plate_y = y + 14;
    const int text_x = card_x + 28;
    const int text_w = plate_x - 20 - text_x;

    FillRoundedRect(surface, card_x, y, card_w, card_h, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, y + 3, card_w - 6, card_h - 6, 15, kCard);

    m_font.Draw(surface, text_x, y + 18, kFontSmall, kSubtle,
                Tr(i18n::StringId::InstallOpenAddress), text_w);

    const util::QrCode &qr = InstallerQrCode();
    const std::string address = InstallerAddressText();
    if (qr.Empty() || address.empty()) {
        /* The server did not come up (or its address cannot be encoded): there is nothing to open,
           and the two console-side entries are still there -- unless this is the upload screen,
           where the page *is* the entry.  The line says which screen it is on, so it stays honest
           in both. */
        m_font.DrawBold(surface, text_x, y + 56, kFontBody, kWarn,
                        Tr(i18n::StringId::InstallNoPage), text_w);
        return y + card_h;
    }

    /* The address is the point of this block, so it gets the biggest text on the screen; a long one
       (an IPv6 literal, say) wraps onto a second line rather than being cut, because a truncated
       address is an address the player cannot type. */
    const std::string shown = m_font.Fit(address, kFontHeading + 8, text_w, 2);
    m_font.DrawBold(surface, text_x, y + 52, kFontHeading + 8, kHeader, shown.c_str(), text_w);

    /* Which file the page is for.  An unfinished upload turns "pick a file" into "pick *this*
       file": the console is holding the bytes of the last attempt, and the page is where the
       player lets it carry on. */
    const bool resuming = for_upload && m_have_pending_task &&
                          m_pending_task.source == install::TaskSource::HttpUpload;
    const i18n::StringId hint = for_upload
                                    ? (resuming ? i18n::StringId::InstallPageResumeHint
                                                : i18n::StringId::InstallPagePickHint)
                                    : i18n::StringId::InstallPagePasteHint;
    m_font.Draw(surface, text_x, y + 122, kFontSmall, kSubtle, Tr(hint), text_w);
    /* The waiting line: a filled dot, the way the app's other "this is a state, not a sentence"
       lines are drawn, and the one thing the player needs to know while nothing happens.  While an
       upload is being read, the line says so and turns into the countdown once the browser has been
       silent for two seconds -- the same rule as the progress screen's footer. */
    std::string waiting_text = Tr(i18n::StringId::InstallWaiting);
    bool waiting_warn = false;
    if (for_upload && m_upload_reading) {
        int wait_seconds = 0;
        if (m_upload_source != nullptr && m_upload_source->PageWaitSeconds(&wait_seconds)) {
            char text[96];
            std::snprintf(text, sizeof(text), Tr(i18n::StringId::ProgressWaitBrowser),
                          wait_seconds);
            waiting_text = text;
            waiting_warn = true;
        } else {
            waiting_text = Tr(i18n::StringId::PageUploadInfo);
        }
    }
    const int dot_centre_y = y + 172;
    FillRoundedRect(surface, text_x, dot_centre_y - 6, 12, 12, 6, kHeader);
    m_font.DrawBold(surface, text_x + 24,
                    dot_centre_y - m_font.LineHeight(kFontBody) / 2, kFontBody,
                    waiting_warn ? kWarn : kText,
                    m_font.Fit(waiting_text, kFontBody, text_w - 24, 1).c_str());

    /* The code sits on its own white plate: a scanner wants a quiet zone, and the plate is what
       gives it one without shrinking the code to a size nobody can scan across a room. */
    FillRoundedRect(surface, plate_x, plate_y, plate_side, plate_side, 14,
                    Color{0xFF, 0xFF, 0xFF, 0xFF});
    StrokeRect(surface, plate_x, plate_y, plate_side, plate_side, 2, kBorder);
    const int module = plate_side / (qr.Size() + 4); /* + 2 modules of quiet zone per side */
    const int code_side = qr.Size() * module;
    const int code_x = plate_x + (plate_side - code_side) / 2;
    const int code_y = plate_y + (plate_side - code_side) / 2;
    /* The painter draws its own white margin one module wide, so it is asked to start one module
       up and left of the code: the plate is white too, which makes that margin invisible. */
    DrawQrCode(surface, qr, code_x - module, code_y - module, module);
    const std::string caption = Tr(i18n::StringId::InstallScanQr);
    m_font.Draw(surface, plate_x + (plate_side - m_font.Measure(caption, kFontSmall)) / 2,
                plate_y + plate_side + 6, kFontSmall, kSubtle, caption.c_str());
    return y + card_h;
}

void App::DrawInstallUrlNotes(Surface surface, int y, bool with_range) {
    /* Small print at the bottom of the screen: what an address has to satisfy, and what this app
       installs.  Both are known before anything is typed -- the alternative was telling the player
       after the address had already been refused. */
    if (with_range) {
        m_font.Draw(surface, kMargin, y, kFontSmall, kSubtle,
                    Tr(i18n::StringId::InstallUrlRangeNote));
    }
    const std::string formats = Tr(i18n::StringId::InstallFormats);
    m_font.Draw(surface, surface.width - kMargin - m_font.Measure(formats, kFontSmall), y,
                kFontSmall, kSubtle, formats.c_str());
}

std::string App::InstallErrorText() const {
    /* Three of the download source's codes have wording of their own -- "connect first", "this
       address cannot be resumed from" (a 200, or a Content-Range we cannot read), and "the address
       answered with this status" (404 and friends, where the number is the useful part); anything
       else is the downloader's own sentence, which is the honest thing to show. */
    if (m_install_error == install::kProbeErrorNoNetwork) {
        return Tr(i18n::StringId::InstallNoNetwork);
    }
    if (m_install_error == install::kProbeErrorBadUrl) {
        return Tr(i18n::StringId::InstallBadUrl);
    }
    if (m_install_error == install::kProbeErrorNotRange) {
        return Tr(i18n::StringId::InstallRangeUnsupported);
    }
    if (m_install_error.rfind(install::kProbeErrorHttpStatusPrefix, 0) == 0) {
        return i18n::Format(
            i18n::StringId::InstallHttpStatus,
            m_install_error.substr(std::strlen(install::kProbeErrorHttpStatusPrefix)).c_str());
    }
    /* The installer's own code for "a game is running" comes back through this screen too (the run
       returns to the address screen so the player can close the game and press A again), and it is
       not a sentence: without this it reached the strip as the literal "game-running". */
    if (m_install_error == install::kErrorGameRunning) {
        return Tr(i18n::StringId::InstallGameRunning);
    }
    return m_install_error;
}

void App::EnterInstallUrl() {
    m_install_origin = InstallScreen::Url;
    m_install_error.clear();
    m_install_screen = InstallScreen::Url;
    RefreshNetworkState();
}

/* The upload source: the same question about the network, because the page is served over it, and
   nothing else -- there is no console-side control on this screen at all. */
void App::EnterInstallUpload() {
    m_install_origin = InstallScreen::Upload;
    m_install_error.clear();
    m_install_screen = InstallScreen::Upload;
    RefreshNetworkState();
    Trace("install: source = upload");
}

void App::RefreshNetworkState() {
    bool has = false;
    std::string error;
    if (!install::HasNetworkAddress(&has, &error) || !has) {
        /* Either the service would not open or the console has no address: both close the two ways
           in, so the screen says "connect first" and the log keeps the reason. */
        has = false;
    }
    m_install_no_network = !has;
    m_install_error.clear();
    if (error.empty()) {
        Trace("install: address screen network=%d", has ? 1 : 0);
    } else {
        Trace("install: address screen network=%d (%s)", has ? 1 : 0, error.c_str());
    }
}

/* The confirm screen: what this file is, before anything is written. */
void App::RenderStartupCheck(Surface surface) {
    m_actions.clear();
    const int card_x = 240;
    const int card_top = kHeaderHeight + 70;
    const int card_w = surface.width - card_x * 2;
    const int card_h = 330;
    FillRoundedRect(surface, card_x, card_top, card_w, card_h, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_w - 6, card_h - 6, 15, kCard);
    const int x = card_x + 36;
    const int width = card_w - 72;
    m_font.DrawBold(surface, x, card_top + 28, kFontTitle, kText,
                    m_font.Fit(Tr(i18n::StringId::StartupCheckTitle), kFontTitle, width, 1).c_str(),
                    width);
    std::string body;
    if (m_startup.decision.action == env::StartupAction::ReportOnly) {
        body = Tr(i18n::StringId::StartupUnknownPath);
    } else {
        body = i18n::Format(i18n::StringId::StartupCheckBody,
                            m_startup.current_path.c_str());
    }
    /* `Fit` decides whether the line count fits; the width argument on `Draw` is what actually
       wraps the text.  Without it the finished sentence ran past the card's right edge. */
    m_font.Draw(surface, x, card_top + 94, kFontBody, kSubtle,
                m_font.Fit(body, kFontBody, width, 2).c_str(), width);
    if (!m_startup_error.empty()) {
        const std::string error =
            m_startup_repaired
                ? m_startup_error
                : i18n::Format(i18n::StringId::StartupFailed, m_startup_error.c_str());
        m_font.Draw(surface, x, card_top + 168, kFontSmall, kWarn,
                    m_font.Fit(error, kFontSmall, width, 2).c_str(), width);
    }
    if (!m_startup_repaired && m_startup.decision.action != env::StartupAction::ReportOnly) {
        const Rect button{x, card_top + card_h - 92, width, 64};
        FillRoundedRect(surface, button.x, button.y, button.w, button.h, 14, kHeader);
        DrawCenteredLabel(surface, m_font, button, kFontBody, kOnHeader,
                          Tr(i18n::StringId::StartupRepair));
        m_actions.push_back({kActionStartupRepair, button, true});
    } else if (m_startup_repaired) {
        m_font.DrawBold(surface, x, card_top + card_h - 96, kFontBody, kWarn,
                        Tr(i18n::StringId::StartupRepairedManual));
    }
    /* The footer's Ⓑ is drawn by RenderFooter; give it the same touch target (and the same
       layout function) every other page's back hint has. */
    const int footer_label = m_font.Measure(Tr(i18n::StringId::LabelExit), kFontSmall);
    const FooterHintLayout footer =
        LayoutFooterHint(surface.height, m_font.LineHeight(kFontSmall), footer_label);
    m_actions.push_back({kActionFooterBack, footer.hit, true});
}

void App::RepairStartup() {
    if (m_sd == nullptr) {
        return;
    }
    std::string error;
    if (!env::ApplyStartupRepair(*m_sd, m_startup, m_log, &error)) {
        m_startup_error = error;
        Trace("startup: repair failed: %s", error.c_str());
        return;
    }
    m_startup_repaired = true;
    Trace("startup: repaired (%s)", m_startup.decision.reason.c_str());
    if (env::SetNextLoadToStandard()) {
        Trace("startup: next load set to the standard path; exiting");
        m_startup_restart = true;
        m_should_exit = true;
    } else {
        m_startup_error.clear();
        Trace("startup: repaired, but the host cannot chain-load; asking for a manual restart");
    }
}

void App::RenderInstallConfirm(Surface surface) {
    m_actions.clear();
    const int card_x = 180;
    const int card_top = kHeaderHeight + 40;
    const int card_width = surface.width - card_x * 2;
    /* The second name is a line of its own under the title, so the card grows with it: the rows
       below (and the note under them, which can wrap to two lines) keep their room instead of
       being pushed into the card's edge. */
    const bool second_name = !m_install_status.game_name_alt.empty();
    const int name_block = second_name ? 44 : 0;
    const int card_height = 400 + name_block;
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);
    /* The cover goes in first: everything the card says is laid out around the room it leaves. */
    const int text_width = DrawPackageCover(surface, card_x, card_top, card_width, card_height);

    const install::PackageInfo &package = m_game_install->Package();
    m_font.Draw(surface, card_x + 36, card_top + 28, kFontSmall, kSubtle,
                Tr(i18n::StringId::ConfirmStart));
    const std::string title = m_install_status.game_name.empty() ? package.cnmt.title_id
                                                                : m_install_status.game_name;
    /* A long name steps down a size or two instead of being cut: this line is the name of the
       thing being installed, and the cover at the card's right leaves a fixed 630 px for it -- a
       name like "Animal Crossing: New Horizons" misses that by a few pixels at the full size and
       would otherwise always end in an ellipsis.  Below the floor a name that is still too long
       is ellipsized, the way every other line in this UI handles it. */
    int title_size = kFontTitle;
    while (title_size > 28 && m_font.Measure(title.c_str(), title_size) > text_width) {
        title_size -= 2;
    }
    m_font.Draw(surface, card_x + 36, card_top + 56, title_size, kText,
                m_font.Fit(title, title_size, text_width, 1).c_str());

    /* The console's own name for the same package, when its language setting says something else
       (install/title_name.hpp): one size down, in the colour the labels use, so it reads as the
       same title in the other language rather than as a second headline. */
    if (second_name) {
        m_font.Draw(surface, card_x + 36, card_top + 112, kFontBody, kSubtle,
                    m_font.Fit(m_install_status.game_name_alt, kFontBody, text_width, 1).c_str());
    }

    const int row_x = card_x + 36;
    const int value_x = card_x + 296;
    /* The values live between the label column and the cover, and one of them can be long -- the
       type row carries what rides along ("Base + Update x1 · DLC x2"), whose English spelling used
       to run under the picture.  Fitted to that room, whichever language it is in. */
    const int value_width = card_x + 36 + text_width - value_x;
    int y = card_top + 136 + name_block;
    const auto row = [&](i18n::StringId label, const std::string &text) {
        m_font.Draw(surface, row_x, y, kFontBody, kSubtle, Tr(label));
        m_font.Draw(surface, value_x, y, kFontBody, kText,
                    m_font.Fit(text, kFontBody, value_width, 1).c_str());
        y += 46;
    };
    if (!m_install_status.game_version.empty()) {
        row(i18n::StringId::ConfirmVersion, m_install_status.game_version);
    }
    switch (package.cnmt.type) {
        case install::MetaType::Application:
            row(i18n::StringId::ConfirmType,
                Tr(i18n::StringId::TypeBase) +
                    (m_install_status.extra_summary.empty()
                         ? std::string()
                         : " + " + m_install_status.extra_summary));
            break;
        case install::MetaType::Patch:
            row(i18n::StringId::ConfirmType,
                Tr(i18n::StringId::TypeUpdate) +
                    (m_install_status.extra_summary.empty()
                         ? std::string()
                         : " + " + m_install_status.extra_summary));
            break;
        case install::MetaType::AddOnContent:
            row(i18n::StringId::ConfirmType, Tr(i18n::StringId::TypeDlc));
            break;
        default:
            row(i18n::StringId::ConfirmType, install::MetaTypeLabel(package.cnmt.type));
            break;
    }
    {
        /* The package's own size, not what is left to fetch: a package whose contents are all
           already on the console has nothing to fetch, and printing "0.0 GB" next to a name and a
           version reads as "this file is empty" (measured on the console). */
        row(i18n::StringId::ConfirmSize,
            util::FormatSize(install::PackageBytes(m_game_install->Task())));
    }
    /* Where this run would write, which is the choice the header's two blocks make -- naming the
       storage here is what makes that choice visible on the card the player actually reads. */
    row(i18n::StringId::ConfirmTarget,
        Tr(m_install_target == install::TargetStorage::Nand ? i18n::StringId::StorageSystem
                                                           : i18n::StringId::StorageSdCard));
    /* Continuing an interrupted run changes what pressing A means, so the card says it: the bytes
       already on the console are not moved again, and the player is not looking at a fresh start. */
    if (m_resumed_bytes > 0) {
        const std::string resumed = i18n::Format(i18n::StringId::ConfirmResumeNote,
                                                util::FormatSize(m_resumed_bytes).c_str());
        m_font.Draw(surface, row_x, y + 10, kFontSmall, kGood,
                    m_font.Fit(resumed, kFontSmall, text_width, 1).c_str());
        y += 28;
    }

    if (TitleIsOurs(package.cnmt.title_id) &&
        install::MetaTypeCarriesGameVersion(package.cnmt.type)) {
        /* Three cases, not two: our game with a supported version is the only one where the agent
           can be installed afterwards.  Our game with a version the agent does not handle has to
           say so; a package that is not ours says nothing at all -- and neither does an add-on or
           a data patch, because the version this note is about is not theirs to change. */
        const bool supported = AgentSupportedFor(package.cnmt.title_id, package.cnmt.version);
        if (supported) {
            m_font.Draw(surface, row_x, y + 10, kFontSmall, kGood,
                        Tr(i18n::StringId::ConfirmAgentNote));
        } else {
            /* Name the version the agent does handle: "the agent will not work with this" alone
               leaves the player with nothing to do next. */
            char note[160];
            std::snprintf(note, sizeof(note), Tr(i18n::StringId::InstallAgentUnsupported),
                          AgentSupportedVersionsFor(package.cnmt.title_id).c_str());
            m_font.Draw(surface, row_x, y + 10, kFontSmall, kWarn, note, text_width);
        }
    }
    /* The card itself is not a button: it is what this package *is*, and the control that starts
       the install is the Ⓐ hint in the footer, which says so.  (A tap anywhere on the card used to
       start the install, which is exactly the "this looks like information but acts like a
       button" problem the control model exists to avoid.) */
}

int App::DrawPackageCover(Surface surface, int card_x, int card_top, int card_w, int card_h) {
    const int full_width = card_w - 72;
    if (m_cover.Empty()) {
        return full_width;
    }
    constexpr int kCover = 200;
    const int x = card_x + card_w - 30 - kCover;
    const int y = card_top + (card_h - kCover) / 2;
    /* A hairline frame, so a bright cover does not bleed into the card around it -- and the
       picture's own corners follow it (radius 10 = the frame's 14 less the 4 px inset), so the
       two read as one rounded object instead of a rounded outline around a square picture. */
    FillRoundedRect(surface, x - 4, y - 4, kCover + 8, kCover + 8, 14, kBorder);
    BlitRgbaScaled(surface, x, y, kCover, kCover, m_cover.rgba.data(), m_cover.width,
                   m_cover.height, 10);
    /* Room left for text, measured from the card's own text inset (36). */
    const int available = x - 24 - (card_x + 36);
    return available > 0 ? available : full_width;
}

void App::RenderInstallResult(Surface surface) {
    m_actions.clear();
    if (m_game_install == nullptr) {
        /* The package never got as far as being described: it did not parse, or a game was
           running.  The two cases the player can act on have wording of their own; anything else
           is quoted the way the installer said it, because a canned "this file is not an install
           package" on a package that failed for some other reason sends the player to look at a
           file that is fine. */
        const int x = kMargin;
        const int wrap = surface.width - kMargin * 2;
        m_font.Draw(surface, x, kHeaderHeight + 60, kFontTitle, kBad,
                    Tr(i18n::StringId::ResultFailed));
        bool canned = true;
        i18n::StringId reason = i18n::StringId::InstallNotAPackage;
        if (m_install_error == "no-content") {
            reason = i18n::StringId::InstallNoContent;
        } else if (m_install_error == install::kErrorGameRunning) {
            reason = i18n::StringId::InstallGameRunning;
        } else if (m_install_error == install::kErrorUploadStopped) {
            reason = i18n::StringId::InstallUploadStopped;
        } else if (!m_install_error.empty() && m_install_error != "not-a-package" &&
                   m_install_error != "no file picked") {
            canned = false;
        }
        if (canned) {
            m_font.Draw(surface, x, kHeaderHeight + 140, kFontBody, kSubtle, Tr(reason));
        } else {
            m_font.Draw(surface, x, kHeaderHeight + 140, kFontBody, kWarn, m_install_error.c_str(),
                        wrap);
        }
        return;
    }
    const install::InstallStatus &status = m_install_status;
    const install::PackageInfo &package = m_game_install->Package();

    /* One screen, three things: what this is, what happened, and what the player can do now.
       The first version of this page used "what / result / now" labels in a table and read like a
       form; a result the player only looks at once should read like a sentence. */
    const int card_x = 140;
    const int card_top = kHeaderHeight + 40;
    const int card_width = surface.width - card_x * 2;
    const int card_height = 330;
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);
    /* The picture the confirm page showed, so the player who walked away during the transfer sees
       the same game here as the one they started -- and, like there, it claims its room first. */
    const int text_width = DrawPackageCover(surface, card_x, card_top, card_width, card_height);

    /* Only `Done` is "installed".  Any other stage reaching this screen means the run stopped
       somewhere -- and calling that "installed" is how a failed finish looked like a success while
       the console still had the old version. */
    i18n::StringId title = i18n::StringId::ResultFailed;
    Color colour = kBad;
    switch (status.stage) {
        case install::InstallStage::Done:
            title = i18n::StringId::ResultInstalled;
            colour = kGood;
            break;
        case install::InstallStage::Skipped:
            /* Decided by the numbers, not by the path that got us here. */
            title = status.installed_version > status.package_version
                        ? i18n::StringId::ResultSkippedNewer
                        : i18n::StringId::ResultSkippedInstalled;
            colour = kGood;
            break;
        default:
            title = i18n::StringId::ResultFailed;
            colour = kBad;
            break;
    }
    m_font.DrawBold(surface, card_x + 40, card_top + 34, kFontTitle, colour, Tr(title));

    /* What this is: name, version, kind -- the same line the progress card carried. */
    const std::string what = PackageSummary();
    m_font.Draw(surface, card_x + 40, card_top + 118, kFontBody + 4, kText,
                m_font.Fit(what, kFontBody + 4, text_width, 1).c_str());

    /* What happened: the same line the browser page shows (InstallOutcomeLine), so the two screens
       cannot describe one run two ways. */
    const std::string happened = InstallOutcomeLine();
    /* Wrapped to the card: a diagnostic like "writing into the placeholder failed (0x307202) at
       offset ..." ran off the screen edge when it was drawn as one line.  The "what now" line
       below then follows however many lines that took (never higher than it used to sit): the
       text column got narrower when the cover arrived, so a long diagnostic can need one line
       more than it did. */
    m_font.Draw(surface, card_x + 40, card_top + 176, kFontBody, kSubtle, happened,
                text_width);
    const int happened_lines = std::max(1, m_font.LineCount(happened, kFontBody, text_width));
    const int now_y = std::max(card_top + 236,
                               card_top + 176 + happened_lines * m_font.LineHeight(kFontBody) + 10);

    /* A failure that kept its work: the bytes are still in their placeholders and the task file
       still describes them, so the one useful thing to say is that nothing is lost.  (Only a
       *transfer* failure keeps them -- a structural failure cleaned up after itself, and the file
       is gone by the time this card is drawn.) */
    if (status.stage != install::InstallStage::Done &&
        status.stage != install::InstallStage::Skipped && m_have_pending_task) {
        m_font.Draw(surface, card_x + 40, now_y, kFontBody, kWarn,
                    Tr(i18n::StringId::ResultTaskKept), text_width);
    }

    /* What now: only about the agent, and only when it is true.  An add-on or a data patch is not
       a version question at all -- it leaves the game's version where it was -- so this run has
       nothing to say about the agent either way. */
    if ((status.stage == install::InstallStage::Done ||
         status.stage == install::InstallStage::Skipped) &&
        install::MetaTypeCarriesGameVersion(package.cnmt.type)) {
        /* The same "what did this run leave behind" question the follow-up asks: the newest
           version-carrying meta of the container (see RunAgentFollowUp). */
        const std::uint32_t effective = status.stage == install::InstallStage::Skipped
                                            ? status.installed_version
                                            : status.effective_version;
        if (AgentSupportedFor(package.cnmt.title_id, effective)) {
            /* The agent is normally in place by the time this screen is up: it was installed by
               the step that followed the game (RunAgentFollowUp), which is the whole point of that
               policy.  So this line reports what that step did -- and when it could not write
               anything, it says so in the colour warnings use, because a game without its agent is
               not the result the player asked for.  "The agent can be installed now" stays as the
               wording for the case where the step was never reached: it is the one line that tells
               the player what to do instead. */
            if (m_agent_follow_up == AgentFollowUp::Installed) {
                m_font.Draw(surface, card_x + 40, now_y, kFontBody, kGood,
                            Tr(i18n::StringId::ResultAgentFollowUpInstalled));
            } else if (m_agent_follow_up == AgentFollowUp::Failed) {
                m_font.Draw(surface, card_x + 40, now_y, kFontBody, kWarn,
                            Tr(i18n::StringId::ResultAgentFollowUpFailed), text_width);
            } else if (AgentInstalled()) {
                m_font.Draw(surface, card_x + 40, now_y, kFontBody, kGood,
                            Tr(i18n::StringId::ResultAgentAlreadyInstalled));
            } else {
                m_font.Draw(surface, card_x + 40, now_y, kFontBody, kGood,
                            Tr(i18n::StringId::ResultAgentReady));
            }
        } else if (TitleIsOurs(package.cnmt.title_id)) {
            /* The same line as the confirm page, formatted the same way: this string carries the
               versions the agent does handle, so it is a format, not a sentence. */
            char note[160];
            std::snprintf(note, sizeof(note), Tr(i18n::StringId::InstallAgentUnsupported),
                          AgentSupportedVersionsFor(package.cnmt.title_id).c_str());
            m_font.Draw(surface, card_x + 40, now_y, kFontBody, kWarn, note, text_width);
        }
    }
    /* A package whose content we had to transform (a cartridge dump's distribution bit) used to
       carry a "the full record is in log.txt" line here.  It told the player nothing they could
       act on -- the transformation is what makes the install work at all -- and the log keeps the
       record either way, so the line is gone. */
}

void App::RenderProgress(Surface surface) {
    /* Nothing on this page is pressable and no keys are handled while it is up (the engine owns
       the thread), so it carries no controls and no chrome (see AddChromeActions). */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> rows;
    rows.push_back({nullptr,
                    Tr(m_progress_uninstall ? i18n::StringId::ProgressUninstalling
                                            : i18n::StringId::ProgressInstalling),
                    kText, 1, kFontTitle});
    rows.push_back({nullptr, Tr(i18n::StringId::ProgressNote), kSubtle, 2});
    if (!m_progress_step.empty()) {
        std::string current = m_progress_step;
        if (m_progress_total > 0) {
            current += " (" + std::to_string(m_progress_index) + "/" +
                       std::to_string(m_progress_total) + ")";
        }
        rows.push_back({Tr(i18n::StringId::LabelProgressFile), current, kText, 1});
    }
    DrawCard(page, page_top, page_bottom, width, value_width, nullptr, rows, kRowGap, false,
             kCardPadBottom);
}

void App::RenderResult(Surface surface) {
    /* One line about what happened, one line of consequence, one button.  Details keep the
       raw error text for us. */
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> rows;
    rows.push_back({nullptr,
                    Tr(m_result_forwarder
                           ? (m_result_ok ? i18n::StringId::ResultForwarderRemoved
                                          : i18n::StringId::ResultForwarderRemoveFailed)
                           : (m_result_uninstall
                                  ? (m_result_ok ? i18n::StringId::ResultUninstallOk
                                                 : i18n::StringId::ResultUninstallFailed)
                                  : (m_result_ok ? i18n::StringId::ResultSuccess
                                                 : i18n::StringId::ResultFailure))),
                    m_result_ok ? kGood : kBad, 1, kFontTitle});
    rows.push_back({nullptr,
                    /* On the result page the files are already in place, so the note speaks
                       about the present ("restart the game") instead of "once installed". */
                    Tr(m_result_forwarder
                           ? (m_result_ok ? i18n::StringId::ResultForwarderRemovedNote
                                          : i18n::StringId::SubRetry)
                           : (m_result_ok
                                  ? (m_result_uninstall ? i18n::StringId::ConfirmUninstallNote
                                                        : i18n::StringId::ResultReadyNote)
                                  : i18n::StringId::SubRetry)),
                    kSubtle, 2});
    if (!m_result_ok && !m_result_error.empty()) {
        rows.push_back({nullptr, m_result_error, kWarn, 2});
        /* This signature once meant "the path buffer sat near the end of a mapping" (fixed in
           util::FsPath); the hint stays as the safety net, because restarting the manager was
           the action that always recovered the session, whatever the cause turned out to be. */
        if (install::IsStaleSessionError(m_result_error)) {
            rows.push_back({nullptr, Tr(i18n::StringId::ResultRelinkHint), kWarn, 2});
        }
    }
    /* Same shape as the confirmation pages: the card sits at the top, the action button stays
       at the bottom.  Centring a three-line card just left a hole at the top, and the card's
       own title only repeated the headline underneath it. */
    const int area_bottom = page_bottom - kHomeSecondaryHeight - kCardGap;
    DrawCard(page, page_top, area_bottom, width, value_width, nullptr, rows, kRowGap, false,
             kCardPadBottom);

    m_actions.clear();
    const int inner = surface.width - kHomeMargin * 2;
    const int top = surface.height - kFooterHeight - kCardGap - kHomeSecondaryHeight;
    /* One action, one full-width button: a half-width button next to empty space reads as a
       missing control. */
    m_actions.push_back(
        {kActionPrimary, Rect{kHomeMargin, top, inner, kHomeSecondaryHeight}, true});
    for (const Action &action_item : m_actions) {
        const int thickness = m_focus == action_item.id ? 6 : 3;
        FillRoundedRect(surface, action_item.rect.x, action_item.rect.y, action_item.rect.w,
                        action_item.rect.h, 18, kHeader);
        FillRoundedRect(surface, action_item.rect.x + thickness, action_item.rect.y + thickness,
                        action_item.rect.w - thickness * 2, action_item.rect.h - thickness * 2,
                        18 - thickness, kHeader);
        DrawKeyBadge(surface, m_font, action_item.rect.x + 46,
                     action_item.rect.y + action_item.rect.h / 2, 22, "A", kOnHeader, kHeader);
        const char *label = Tr(i18n::StringId::Done);
        DrawCenteredLabel(surface, m_font, action_item.rect, kFontHeading + 8, kOnHeader, label);
    }
}

}  // namespace acnh_manager::ui
