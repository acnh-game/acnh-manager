#include "update.hpp"

#include <cstdio>
#include <string>

#include <switch.h>
#include <curl/curl.h>

namespace acnh_manager::net {
namespace {

std::size_t WriteCallback(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    const std::size_t bytes = size * nmemb;
    if (out->size() + bytes > 512 * 1024) {
        return 0; /* 清单不该这么大,直接失败 */
    }
    out->append(ptr, bytes);
    return bytes;
}

bool FileReadable(const std::string &path, std::string *error) {
    FsFileSystem sd{};
    if (R_FAILED(fsOpenSdCardFileSystem(&sd))) {
        if (error != nullptr) {
            *error = "无法挂载 SD 卡";
        }
        return false;
    }
    FsFile file{};
    const Result rc = fsFsOpenFile(&sd, path.c_str(), FsOpenMode_Read, &file);
    if (R_SUCCEEDED(rc)) {
        fsFileClose(&file);
    }
    fsFsClose(&sd);
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            *error = "缺少 CA 文件: " + path;
        }
        return false;
    }
    return true;
}

}  // namespace

UpdateCheckResult CheckForUpdate(const std::string &url, const std::string &ca_path,
                                 long timeout_seconds) {
    UpdateCheckResult result;
    result.attempted = true;

    if (url.rfind("https://", 0) != 0) {
        result.skipped = true;
        result.reason = "只接受 https 地址";
        return result;
    }
    std::string ca_error;
    if (ca_path.empty() || !FileReadable(ca_path, &ca_error)) {
        result.skipped = true;
        result.reason = ca_path.empty() ? "未配置 CA bundle" : ca_error;
        return result;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.reason = "curl_easy_init 失败";
        return result;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.manifest_text);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "acnh-manager/0.1.0");
    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);
    }
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "网络失败: %s", curl_easy_strerror(code));
        result.reason = buf;
        result.manifest_text.clear();
        return result;
    }
    if (result.http_code != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", result.http_code);
        result.reason = buf;
        result.manifest_text.clear();
        return result;
    }
    result.ok = true;
    result.reason = "已获取发布清单";
    return result;
}

}  // namespace acnh_manager::net
