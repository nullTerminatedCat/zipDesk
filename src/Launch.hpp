#pragma once

#include <string>
#include <vector>

namespace zipdesk {

enum class LaunchAction {
    None,
    Open,
    ExtractHere,
    ExtractToFolder,
    CompressZip,
    Compress7z,
    Register,
    Unregister
};

struct LaunchCommand {
    LaunchAction action = LaunchAction::None;
    std::vector<std::wstring> paths;
};

LaunchCommand parseLaunch(const std::vector<std::wstring>& args);

// 资源管理器会为每个选中文件单独启动一次。返回 false 表示本次启动应退出，由先启动的进程一起压缩。
bool claimShellBatch(LaunchCommand& command);

}  // namespace zipdesk
