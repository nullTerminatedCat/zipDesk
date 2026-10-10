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
        {"打开", "Open"},
        {"新建压缩", "New archive"},
        {"解压", "Extract"},
        {"追加", "Add"},
        {"测试", "Test"},
        {"修复", "Repair"},
        {"复制", "Copy"},
        {"移动", "Move"},
        {"删除", "Delete"},
        {"信息", "Info"},
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
        {"拖入 zip / 7z 即可打开，拖入文件或文件夹则新建压缩", "Drop zip, 7z or rar to open. Drop files to compress."},
        {"打开压缩包", "Open archive"},
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
