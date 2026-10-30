#include "Lang.hpp"

#include <QSettings>

namespace zipdesk {
namespace {

const char* englishOf(const QString& chinese) {
    struct Item {
        const char* zh;
        const char* en;
    };
    static const Item items[] = {
        {"返回", "Back"},
        {"首页", "Home"},
        {"打开", "Open"},
        {"新建压缩", "New archive"},
        {"解压", "Extract"},
        {"智能解压", "Smart extract"},
        {"追加", "Add"},
        {"添加", "Add"},
        {"测试", "Test"},
        {"修复", "Repair"},
        {"复制", "Copy"},
        {"移动", "Move"},
        {"删除", "Delete"},
        {"信息", "Info"},
        {"更多", "More"},
        {"工具", "Tools"},
        {"预览", "Preview"},
        {"测试完整性", "Test integrity"},
        {"修复压缩包", "Repair archive"},
        {"抢救可读取文件", "Recover readable files"},
        {"复制所选到…", "Copy selection to..."},
        {"移动所选到…", "Move selection to..."},
        {"删除所选", "Delete selection"},
        {"解压到…", "Extract to..."},
        {"解压到同名文件夹", "Extract to named folder"},
        {"解压所选到…", "Extract selection to..."},
        {"解压所选到同名文件夹", "Extract selection to named folder"},
        {"追加文件", "Add files"},
        {"追加文件夹", "Add folder"},
        {"添加文件", "Add files"},
        {"添加文件夹", "Add folder"},
        {"设置", "Settings"},
        {"钉在前台", "Pin on top"},
        {"取消钉住", "Unpin"},
        {"最小化", "Minimize"},
        {"最大化", "Maximize"},
        {"还原", "Restore"},
        {"关闭", "Close"},
        {"集成到资源管理器", "Integrate with Explorer"},
        {"取消资源管理器集成", "Remove Explorer integration"},
        {"把压缩包放在这里", "Drop an archive here"},
        {"拖入 zip / 7z / rar / tar / gz / xz / iso / zipx / .001 即可打开，拖入文件或文件夹则新建压缩", "Drop zip, 7z, rar, tar, gz, xz, iso, zipx or .001 volumes to open. Drop files to compress."},
        {"打开压缩包", "Open archive"},
        {"最近打开", "Recent archives"},
        {"文件收好，随时打开", "A little order for every file"},
        {"压缩包内容", "Archive contents"},
        {"压缩包里还没有文件", "This archive is empty"},
        {"没有找到匹配的文件", "No matching files"},
        {"添加文件，或将文件拖入窗口。", "Add files or drag them into this window."},
        {"试试更短的关键词，或清除搜索查看全部内容。", "Try a shorter keyword, or clear the search to see all files."},
        {"清除搜索", "Clear search"},
        {"搜索整个压缩包", "Search the entire archive"},
        {"复制压缩包路径", "Copy archive path"},
        {"支持 ZIP / 7z / RAR / TAR / GZ / XZ / ISO / ZIPX / 分卷(.001) 浏览与解压 · ZIP / 7z 创建", "Browse and extract ZIP / 7z / RAR / TAR / GZ / XZ / ISO / ZIPX / split (.001) · Create ZIP / 7z"},
        {"%1 项", "%1 items"},
        {"当前显示 %1 项 · 压缩包共 %2 项 · %3", "Showing %1 items · %2 in archive · %3"},
        {"找到 %1 项", "%1 matches"},
        {"清空记录", "Clear history"},
        {"当前窗口打开", "Open here"},
        {"新建窗口打开", "Open in new window"},
        {"查看", "View"},
        {"编辑", "Edit"},
        {"重命名", "Rename"},
        {"复制到...", "Copy to..."},
        {"移动到...", "Move to..."},
        {"分割文件...", "Split file..."},
        {"合并文件...", "Combine files..."},
        {"属性", "Properties"},
        {"注释", "Comment"},
        {"新建文件夹", "New folder"},
        {"新建文件", "New file"},
        {"链接", "Link"},
        {"交替数据流", "Alternate data streams"},
        {"选择文件可预览文本或图片", "Select a file to preview text or an image"},
        {"正在加载预览…", "Loading preview..."},
        {"无法预览此文件", "Unable to preview this file"},
        {"无法预览此图片", "Unable to preview this image"},
        {"文件超过预览上限（2 MB），请解压后查看",
         "File exceeds the 2 MB preview limit. Extract it to view."},
        {"文件超过内存编辑上限（2 MB），请解压到磁盘后用外部编辑器打开。",
         "File exceeds the 2 MB in-memory edit limit. Extract it and open with an external editor."},
        {"青匣 · ZIP / 7Z / RAR / TAR / GZ / XZ / ISO / ZIPX / 分卷", "QingXia · ZIP / 7Z / RAR / TAR / GZ / XZ / ISO / ZIPX / volumes"},
        {"打开压缩包，轻松整理", "Open an archive and get organized"},
        {"拖入压缩包即可查看内容；拖入文件或文件夹即可创建压缩包。", "Drop an archive to browse it, or drop files to create one."},
        {"松开以打开压缩包或新建压缩", "Release to open an archive or create one"},
        {"松开以追加到当前压缩包", "Release to add to this archive"},
        {"拖入压缩包可打开，拖入文件或文件夹可新建压缩", "Drop an archive to open it, or drop files to create one"},
        {"拖入文件可追加到压缩包", "Drop files to add them to the archive"},
        {"上一级", "Up one level"},
        {"使用 Windows 10 经典右键菜单", "Use the Windows 10 classic context menu"},
        {"更改后需重启资源管理器才会生效；不需要管理员权限。",
         "Restart File Explorer for the change to take effect. Administrator rights are not required."},
        {"仅 Windows 11 可用。当前系统不支持此选项。", "Available on Windows 11 only."},
        {"立即重启资源管理器", "Restart Explorer now"},
        {"稍后自行重启", "Restart later"},
        {"经典右键菜单设置已更新。", "Classic context menu setting updated."},
        {"无法更改经典右键菜单设置，请稍后重试。", "Could not change the classic context menu setting. Please try again."},
        {"未能重启资源管理器，请在任务管理器中结束 explorer.exe 后再运行它。",
         "Could not restart Explorer. End explorer.exe in Task Manager, then start it again."},
    };
    for (const Item& item : items) {
        if (chinese == QString::fromUtf8(item.zh)) {
            return item.en;
        }
    }
    return nullptr;
}

}  // namespace

UiLanguage uiLanguage() {
    return QSettings().value(QStringLiteral("ui/language"), QStringLiteral("zh")).toString() == QLatin1String("en")
               ? UiLanguage::English
               : UiLanguage::Chinese;
}

void setUiLanguage(UiLanguage language) {
    QSettings().setValue(QStringLiteral("ui/language"), language == UiLanguage::English ? QStringLiteral("en") : QStringLiteral("zh"));
}

QString translate(const QString& chinese) {
    if (uiLanguage() != UiLanguage::English) {
        return chinese;
    }
    if (const char* english = englishOf(chinese)) {
        return QString::fromUtf8(english);
    }
    return chinese;
}

}  // namespace zipdesk
