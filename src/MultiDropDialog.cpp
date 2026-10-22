#include "MultiDropDialog.hpp"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

MultiDropDialog::MultiDropDialog(int archiveCount, int otherCount, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("拖入了多个项目");
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(420);
    setStyleSheet(
        "QDialog { background: #f5f7fa; color: #20332f; }"
        "QLabel { color: #20332f; }"
        "QLabel#dropTitle { font-size: 15px; font-weight: 600; }"
        "QLabel#dropHint { color: #667b75; font-size: 12px; }"
        "QPushButton { background: #ffffff; color: #20332f; border: 1px solid #dfe7e4;"
        "  border-radius: 8px; padding: 10px 14px; text-align: left; }"
        "QPushButton:hover { background: #edf6f2; border-color: #8cbbae; }"
        "QPushButton:focus { background: #edf6f2; border: 2px solid #147d70; padding: 9px 13px; }"
        "QPushButton#primaryDrop { background: #147d70; color: #ffffff; border-color: #147d70; }"
        "QPushButton#primaryDrop:hover { background: #10685d; }"
        "QPushButton#cancelDrop { color: #667b75; }");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(10);

    QString summary;
    if (otherCount > 0) {
        summary = QString("拖入了 %1 个压缩包和 %2 个其他项目。请选择如何处理：")
                      .arg(archiveCount)
                      .arg(otherCount);
    } else {
        summary = QString("拖入了 %1 个压缩包。请选择如何处理：").arg(archiveCount);
    }
    auto* title = new QLabel(summary, this);
    title->setObjectName("dropTitle");
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* hint = new QLabel("批量解压只处理压缩包；压成新包会把拖入的全部路径一起压缩。", this);
    hint->setObjectName("dropHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addSpacing(4);

    auto addButton = [&](const QString& text, Choice choice, bool primary = false) {
        auto* button = new QPushButton(text, this);
        if (primary) {
            button->setObjectName("primaryDrop");
            button->setDefault(true);
        }
        connect(button, &QPushButton::clicked, this, [this, choice]() { pick(choice); });
        layout->addWidget(button);
        return button;
    };

    addButton("全部智能解压", Choice::SmartExtractAll, true);
    addButton("全部解压到各自同名文件夹", Choice::ExtractToFolders);
    addButton("压成新压缩包（含压缩包文件本身）", Choice::CreateArchive);
    addButton("打开第一个压缩包", Choice::OpenFirst);

    auto* cancel = new QPushButton("取消", this);
    cancel->setObjectName("cancelDrop");
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancel);
}

void MultiDropDialog::pick(Choice choice) {
    m_choice = choice;
    accept();
}
