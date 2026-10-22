#pragma once

/* The pure half of the installer's web page: turning our own text into the page the console hands
   out, and turning a response into bytes.  Nothing here touches a socket, so the host tests can pin
   the parts that are easy to get wrong (escaping, the label substitution, the head's framing). */

#include <cstdio>
#include <string>
#include <string_view>

#include "i18n/strings.hpp"

namespace acnh_manager::net {

/* Text as a JavaScript string literal's contents: backslash, quote and the newline family are
   escaped, and `<` becomes `\u003c` so a label can never end the surrounding <script> block (the
   page is one file, and a stray "</script>" in a translation would break it in a way that is
   invisible until somebody opens the page in that language). */
inline std::string EscapeForJs(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<': out += "\\u003c"; break;
            case '>': out += "\\u003e"; break;
            case '&': out += "\\u0026"; break;
            default:
                /* Everything else goes through as bytes: the page is UTF-8, like the label table. */
                out += c;
                break;
        }
    }
    return out;
}

/* Where the page wants its labels: `data/installer_page.html.bin` carries this token inside its
   script block, and the server replaces it with a JavaScript object literal of the current
   language's text.  A plain token rather than a template language: the page is one file, and this is
   the only thing that ever changes in it. */
inline constexpr const char *kPageLabelsMarker = "__ACNH_LABELS__";
/* The two other things the page cannot know by itself: which language to declare, and the address
   the player typed (the page shows it back so the two screens agree). */
inline constexpr const char *kPageLangMarker = "__ACNH_LANG__";
inline constexpr const char *kPageAddressMarker = "__ACNH_ADDRESS__";
/* And which build the page came from.  A browser keeps the JavaScript it already has until the tab
   is reloaded, so a page left open across a deploy behaves exactly like a page whose upload loop is
   broken -- which is a debugging round nobody should pay twice (2026-10-16).  This identity stays
   the full build stamp in every build; it is compared with /status.json for self-refresh. */
inline constexpr const char *kPageVersionMarker = "__ACNH_VERSION__";
/* What the footer shows and copies: the full stamp for development/dirty builds, or the localized
   application version for a clean release. */
inline constexpr const char *kPageVersionTextMarker = "__ACNH_VERSION_TEXT__";

/* The page with its label placeholder replaced.  `labels_object` is a JavaScript object literal
   (built by the app from the i18n table, one key per label the page shows). */
inline std::string SubstituteLabels(std::string_view page, std::string_view marker,
                                    std::string_view labels_object) {
    std::string out;
    out.reserve(page.size() + labels_object.size());
    std::size_t at = 0;
    for (;;) {
        const std::size_t found = page.find(marker, at);
        if (found == std::string_view::npos) {
            out.append(page.substr(at));
            return out;
        }
        out.append(page.substr(at, found - at));
        out.append(labels_object);
        at = found + marker.size();
    }
}

/* The page's labels, as a JavaScript object literal, straight out of the app's i18n table: the
   page then carries no wording of its own (design section 14, item 15), and a translated label can
   never be out of step with the console's own screens. */
inline std::string BuildPageLabelsJson() {
    struct Label {
        const char *key;
        i18n::StringId id;
    };
    const Label labels[] = {
        {"pageTitle", i18n::StringId::TabInstall},
        {"pageSubtitle", i18n::StringId::AppTitle},
        {"pageStep1", i18n::StringId::PageStep1},
        {"pageChooseFile", i18n::StringId::PageChooseFile},
        {"pageDropHint", i18n::StringId::PageDropHint},
        {"pageFormats", i18n::StringId::InstallFormats},
        {"pageKeepAwake", i18n::StringId::PageKeepAwake},
        {"pageUrlTitle", i18n::StringId::PageUrlTitle},
        {"pageUrlPlaceholder", i18n::StringId::PageUrlPlaceholder},
        {"pageUrlButton", i18n::StringId::PageUrlButton},
        {"pageUrlSent", i18n::StringId::PageUrlSent},
        {"pageUrlFailed", i18n::StringId::PageUrlFailed},
        {"pageQueueTitle", i18n::StringId::PendingTitle},
        {"pageQueueDetail", i18n::StringId::PendingProgress},
        {"pageStageDownload", i18n::StringId::ProgressReceive},
        {"pageStageUpload", i18n::StringId::PageStageUpload},
        {"pageContents", i18n::StringId::ProgressContents},
        {"pageResultDone", i18n::StringId::ResultInstalled},
        {"pageResultSkipped", i18n::StringId::ResultSkippedInstalled},
        {"pageResultFailed", i18n::StringId::ResultFailed},
        {"pageAgentReady", i18n::StringId::PageAgentReady},
        {"pageUploadStarting", i18n::StringId::PageUploadStarting},
        {"pageUploadWaiting", i18n::StringId::PageUploadWaiting},
        {"pageUploadProgress", i18n::StringId::PageUploadProgress},
        {"pageUploadBusy", i18n::StringId::PageUploadBusy},
        {"pageHidden", i18n::StringId::PageHidden},
        {"pageCopyLog", i18n::StringId::PageCopyLog},
        {"pageClearLog", i18n::StringId::PageClearLog},
        {"pageCopied", i18n::StringId::PageCopied},
        {"pageCopyFailed", i18n::StringId::PageCopyFailed},
        {"pagePeerActive", i18n::StringId::PagePeerActive},
        {"pageUploadActive", i18n::StringId::PageUploadActive},
        {"pageWaitOriginal", i18n::StringId::PageWaitOriginal},
        /* The mirrored confirm screen and the header's storage blocks reuse the console's own
           wording, so the two devices cannot drift apart (design section 8). */
        {"pageInstallTo", i18n::StringId::InstallToLabel},
        {"pageConfirmTitle", i18n::StringId::ConfirmStart},
        {"pageConfirmSize", i18n::StringId::ConfirmSize},
        {"pageConfirmTarget", i18n::StringId::ConfirmTarget},
        {"pageStart", i18n::StringId::ConfirmStart},
        {"pageRereadOnSwitch", i18n::StringId::PageRereadOnSwitch},
        {"pageStorageUnavailable", i18n::StringId::PageStorageUnavailable},
        {"pageWaitingPick", i18n::StringId::PageWaitingPick},
        {"pageWaitingStart", i18n::StringId::PageWaitingStart},
        {"pageStarting", i18n::StringId::PageStarting},
        {"pageTargetRefused", i18n::StringId::PageTargetRefused},
        {"pageStartRefused", i18n::StringId::PageStartRefused},
        {"pageReselectToStart", i18n::StringId::PageReselectToStart},
        {"pageRestartUpload", i18n::StringId::PageRestartUpload},
        {"pageUploadDropped", i18n::StringId::PageUploadDropped},
        {"pageBack", i18n::StringId::LabelBack},
        {"pageBackRefused", i18n::StringId::PageBackRefused},
        {"pageUploadInfo", i18n::StringId::PageUploadInfo},
        {"pageDebugTitle", i18n::StringId::PageDebugTitle},
        {"pageDebugMinimize", i18n::StringId::PageDebugMinimize},
        {"pageDebugMaximize", i18n::StringId::PageDebugMaximize},
        {"pageDebugRestore", i18n::StringId::PageDebugRestore},
        {"pageDebugLatest", i18n::StringId::PageDebugLatest},
        {"pageResultTaskKept", i18n::StringId::ResultTaskKept},
        {"pageUploadFailed", i18n::StringId::PageUploadFailed},
        {"pageOffline", i18n::StringId::PageOffline},
        {"pageNoteForeground", i18n::StringId::PageKeepAwake},
        {"pageNoteNoInternet", i18n::StringId::PageNoteNoInternet},
        {"pageNoteHostScreen", i18n::StringId::PageNoteHostScreen},
    };
    std::string out = "{";
    bool first = true;
    for (const Label &label : labels) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "\"";
        out += label.key;
        out += "\":\"";
        out += EscapeForJs(i18n::Text(label.id, i18n::Current()));
        out += "\"";
    }
    out += "}";
    return out;
}

/* One value out of a query string ("offset=123&taskId=ab"), or empty when it is not there.  Plain
   string walking on purpose: the page is the only client, the values are numbers and hex, and a URL
   parser would be more code than the whole route table. */
/* How many bytes of a request body are still on the way when a route answers *before* reading it
   (a refused upload part, an oversized POST).  Answering and closing with those bytes unread makes
   the kernel send an RST instead of a FIN, and the browser then reports a network error instead of
   the answer -- which is how a 409 that exists to make the page re-sync killed the page's upload
   loop instead (measured 2026-10-16).  So the server swallows this many bytes first, and this is
   the arithmetic it uses: whatever the head said, minus what is already in hand, and never below
   zero (a request with no body drains nothing). */
inline std::size_t DrainBytes(std::size_t body_start, std::size_t buffered, std::size_t body_length) {
    const std::size_t have = buffered > body_start ? buffered - body_start : 0;
    return body_length > have ? body_length - have : 0;
}

inline std::string QueryValue(std::string_view query, std::string_view key) {
    std::size_t at = 0;
    while (at <= query.size()) {
        const std::size_t end = query.find('&', at);
        const std::string_view pair =
            query.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == key) {
            return std::string(pair.substr(equals + 1));
        }
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }
    return std::string();
}

/* The response head, framed exactly the way the browser needs it.  `Connection: close` on purpose:
   one request per connection keeps the server's state machine to one pass (see http_server.hpp). */
inline std::string BuildResponseHead(int status, const char *status_text, std::string_view content_type,
                                     std::size_t content_length) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "HTTP/1.1 %d %s\r\nContent-Type: %.*s\r\nContent-Length: %zu\r\n"
                  /* Nothing this console serves is worth keeping: the page is replaced with the
                     binary, the status changes every second, and the cover belongs to the package in
                     hand.  `no-store` is the one that matters; `no-cache`/`Pragma`/`Expires` are the
                     belt for clients that only understand the older spelling (a phone browser or an
                     embedded WebView that keeps a page forever is exactly what stale code looks
                     like). */
                  "Connection: close\r\n"
                  "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                  "Pragma: no-cache\r\nExpires: 0\r\n\r\n",
                  status, status_text, static_cast<int>(content_type.size()), content_type.data(),
                  content_length);
    return buffer;
}

}  // namespace acnh_manager::net
