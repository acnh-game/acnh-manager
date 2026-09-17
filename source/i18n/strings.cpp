#include "strings.hpp"

namespace acnh_manager::i18n {
namespace {

struct Entry {
    const char *zh;
    const char *en;
};

/* 与 StringId 一一对应;顺序必须一致(由 tests 里的覆盖检查兜住)。 */
constexpr Entry kStrings[] = {
    {"ACNH-Manager", "ACNH-Manager"},
    {"状态", "Status"},
    {"设置", "Settings"},
    {"游戏", "Game"},
    {"覆盖设置", "Override"},
    {"安装现状", "Installed"},
    {"发布清单", "Manifest"},
    {"系统版本", "System"},
    {"运行模式", "Mode"},
    {"游戏是否运行", "Game running"},
    {"游戏版本号", "Game version"},
    {"更新包内容 id", "Update content id"},
    {"运行中构建号", "Running build id"},
    {"覆盖键提示", "Override key advice"},
    {"现有覆盖文件", "Existing override files"},
    {"旧金手指文件", "Legacy cheat file"},
    {"清单状态", "Manifest state"},
    {"门控结果", "Gate result"},
    {"取数异常", "Collection problems"},
    {"A 刷新   L/R 切换页面   B 退出", "A refresh   L/R switch page   B exit"},
    {"A 切换语言   L/R 切换页面   B 退出", "A toggle language   L/R switch page   B exit"},
    {"界面语言", "Language"},
    {"简体中文", "简体中文"},
    {"English", "English"},
    {"(不可用)", "(unavailable)"},
    {"(游戏未运行)", "(game not running)"},
    {"无", "none"},
    {"有", "present"},
    {"无", "absent"},
    {"未内置(等待发布导入)", "not embedded (pending release import)"},
    {"检测到旧金手指文件,与 agent 可能冲突,建议移入备份",
     "Legacy cheat file detected; it may conflict with the agent. Consider moving it to a backup."},
    {"按 A 重新采集环境", "Press A to re-collect the environment"},
    {"安装确认", "Install"},
    {"结果", "Result"},
    {"卸载", "Uninstall"},
    {"将要写入的文件", "Files to write"},
    {"payload 目录", "Payload folder"},
    {"干跑模式", "Dry run"},
    {"清单来源", "Manifest source"},
    {"开启(只校验不写盘)", "on (verify only)"},
    {"关闭(真正写入)", "off (write to SD)"},
    {"开发用清单文件", "dev manifest file"},
    {"A 开始   B 返回", "A run   B back"},
    {"A 重新采集   B 返回", "A re-collect   B back"},
    {"按下 A 开始安装", "Press A to install"},
    {"按下 A 卸载并回滚", "Press A to uninstall and roll back"},
    {"安装成功", "Install succeeded"},
    {"安装失败", "Install failed"},
    {"没有可用的发布清单:请放入开发用清单文件后重试",
     "No manifest available: add the dev manifest file and retry"},
    {"将删除 state.json 记录中的文件;被修改过的文件会被拒绝删除",
     "Files recorded in state.json will be removed; modified files are refused"},
    {"按 B 退出", "Press B to exit"},
};

constexpr std::size_t kCount = sizeof(kStrings) / sizeof(kStrings[0]);

}  // namespace

const char *Text(StringId id, Language language) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kCount) {
        return "";
    }
    return language == Language::ZhHans ? kStrings[index].zh : kStrings[index].en;
}

unsigned StringCount() { return static_cast<unsigned>(kCount); }

}  // namespace acnh_manager::i18n
