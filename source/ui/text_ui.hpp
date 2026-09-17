#pragma once

/* 文本界面(默认形态)。

   为什么存在:在 hbl/applet 环境下,自制程序自己新建 framebuffer 的图形界面在本机上会
   让加载器进程崩溃(见 docs/device-acceptance.md 的崩溃调查记录);而 libnx 控制台路径
   已被 M0/M1-A 两版实测证明可用。所以默认走控制台文本界面,图形界面用开关启用:
   `/switch/ACNH-Manager/ui-graphics` 存在时尝试图形界面。

   控制台字体是 ASCII 8×8,因此界面文案只用英文;中文说明留在 log.txt 里。 */

#include <switch.h>

namespace acnh_manager {
class Log;
}

namespace acnh_manager::ui {

class TextUi {
public:
    /* 返回 false 表示无法初始化控制台。 */
    bool Init();
    void Exit();
    /* 主循环:按 B 或 + 退出。 */
    void Run(acnh_manager::Log *log, FsFileSystem &sd);
};

}  // namespace acnh_manager::ui
