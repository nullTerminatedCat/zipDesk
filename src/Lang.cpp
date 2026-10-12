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
        {"拖入 zip / 7z / rar 即可打开，拖入文件或文件夹则新建压缩", "Drop zip, 7z or rar to open. Drop files to compress."},
        {"打开压缩包", "Open archive"},
        {"最近打开", "Recent archives"},
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
        {"青匣 · ZIP / 7Z / RAR", "QingXia · ZIP / 7Z / RAR"},
        {"打开压缩包，轻松整理", "Open an archive and get organized"},
        {"拖入压缩包即可查看内容；拖入文件或文件夹即可创建压缩包。", "Drop an archive to browse it, or drop files to create one."},
        {"松开以打开压缩包或新建压缩", "Release to open an archive or create one"},
        {"松开以追加到当前压缩包", "Release to add to this archive"},
        {"拖入压缩包可打开，拖入文件或文件夹可新建压缩", "Drop an archive to open it, or drop files to create one"},
        {"拖入文件可追加到压缩包", "Drop files to add them to the archive"},
        {"上一级", "Up one level"},
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
